import {
  P_MAX,
  EXPORT_RESERVE_W,
  TAU_UP,
  TAU_DOWN,
} from "./constants";
import { getStore, addHistoryEntry, type EspReport } from "./store";
import { insertReading, pruneIfNeeded } from "./db";

let requestCount = 0;

export interface ReportResult {
  duty_pct: number;
  mode: "auto" | "manual";
  ack: boolean;
}

export function processReport(report: EspReport): ReportResult {
  const store = getStore();
  const now = Date.now();

  // --- Step 1: Calculate measured export ---
  const measuredExport = report.power_w < 0 ? -report.power_w : 0;
  const gridImport = report.power_w > 0 ? report.power_w : 0;

  // --- Step 2: Calculate GROSS export (key insight from PLAN.md) ---
  // gross = measured export + current heater power
  // This removes feedback loop: gross only changes with solar/house consumption
  const grossExport = measuredExport + store.pApplied;

  // --- Step 3: Add gross to circular buffer ---
  store.grossBuffer[store.bufferIndex] = grossExport;
  store.bufferIndex = (store.bufferIndex + 1) % store.grossBuffer.length;
  if (store.bufferIndex === 0) store.bufferFull = true;

  // --- Step 4: Calculate target power ---
  let pTarget: number;

  if (store.mode === "manual") {
    // MANUAL: direct duty from dashboard, no EMA
    pTarget = (store.manualDuty / 100) * P_MAX;
    store.pApplied = pTarget; // Immediate, no filter
  } else {
    // AUTO: proportional targeting based on gross export average
    const avgGross = getAverageGross();
    pTarget = avgGross - EXPORT_RESERVE_W;
    pTarget = Math.max(0, Math.min(pTarget, P_MAX));

    // --- Step 5: EMA filter for smooth transitions ---
    const dt =
      store.lastRequestMs > 0 ? (now - store.lastRequestMs) / 1000 : 1;
    const tau = pTarget > store.pApplied ? TAU_UP : TAU_DOWN;
    const k = dt / (tau + dt);
    store.pApplied = store.pApplied + k * (pTarget - store.pApplied);
    store.pApplied = Math.max(0, Math.min(store.pApplied, P_MAX));
  }

  store.lastRequestMs = now;

  // --- Step 6: Calculate duty cycle ---
  const dutyPct = Math.max(0, Math.min((store.pApplied / P_MAX) * 100, 100));

  // --- Step 7: Update last report ---
  store.lastReport = {
    powerW: report.power_w,
    uptimeS: report.uptime_s,
    wifiRssi: report.wifi_rssi,
    freeHeap: report.free_heap,
    currentDutyPct: report.current_duty_pct,
    safeMode: report.safe_mode,
    receivedAt: now,
  };

  // --- Step 8: Save history ---
  const entry = {
    timestamp: now,
    power_w: report.power_w,
    export_w: measuredExport,
    gross_export_w: grossExport,
    heater_w: store.pApplied,
    duty_pct: dutyPct,
    grid_import_w: gridImport,
  };

  addHistoryEntry(entry);

  // Persist to SQLite for long-term storage
  insertReading({
    ts: now,
    grid_import_w: gridImport,
    export_w: measuredExport,
    heater_w: store.pApplied,
    avg_export_w: grossExport,
    auto_mode: store.mode === "auto",
    p_target: pTarget,
    p_applied: store.pApplied,
  });

  // Periodic DB size check
  requestCount++;
  if (requestCount % 500 === 0) {
    pruneIfNeeded();
  }

  return {
    duty_pct: Math.round(dutyPct * 10) / 10,
    mode: store.mode,
    ack: true,
  };
}

function getAverageGross(): number {
  const store = getStore();
  const count = store.bufferFull
    ? store.grossBuffer.length
    : store.bufferIndex;
  if (count === 0) return 0;

  let sum = 0;
  for (let i = 0; i < count; i++) {
    sum += store.grossBuffer[i];
  }
  return sum / count;
}
