import { P_MAX, EXPORT_RESERVE_W } from "./constants";
import { getStore, addHistoryEntry, type EspReport } from "./store";
import { insertReading, pruneIfNeeded } from "./db";

let requestCount = 0;
let lastIncreaseMs = 0;

// 3-priority algorithm constants (matching simulation.html)
const ALPHA_DOWN = 0.7; // Fast filter for decrease (70% new, 30% old)
const ALPHA_UP = 0.4; // Filter for increase (40% new, 60% old)
const INCREASE_INTERVAL_MS = 2000; // Increase every 2s
const MAX_STEP_W = 200; // Max change per tick [W]

export interface ReportResult {
  duty_pct: number;
  mode: "auto" | "manual";
  ack: boolean;
}

export function processReport(report: EspReport): ReportResult {
  const store = getStore();
  const now = Date.now();

  const measuredExport = report.power_w < 0 ? -report.power_w : 0;
  const gridImport = report.power_w > 0 ? report.power_w : 0;

  // Gross export = measured + heater (removes feedback loop)
  const grossExport = measuredExport + store.pApplied;

  // Add gross to circular buffer
  store.grossBuffer[store.bufferIndex] = grossExport;
  store.bufferIndex = (store.bufferIndex + 1) % store.grossBuffer.length;
  if (store.bufferIndex === 0) store.bufferFull = true;

  let pTarget = store.pApplied;
  const prevApplied = store.pApplied;

  if (store.mode === "manual") {
    pTarget = (store.manualDuty / 100) * P_MAX;
    store.pApplied = pTarget;
  } else {
    // ============================================================
    // 3-PRIORITY ALGORITHM (same as simulation.html)
    // ============================================================

    // PRIORITY 1: IMPORT detected → immediate cut, NO filter
    // If drawing from grid, cut heater by import + reserve immediately
    if (gridImport > 0) {
      const cut = gridImport + EXPORT_RESERVE_W;
      store.pApplied = Math.max(0, store.pApplied - cut);
    }
    // PRIORITY 2: Export below reserve → fast proportional reduction
    // Export exists but too small - reduce proportionally to deficit
    else if (measuredExport < EXPORT_RESERVE_W) {
      const deficit = EXPORT_RESERVE_W - measuredExport;
      const target = Math.max(0, store.pApplied - deficit);
      store.pApplied = ALPHA_DOWN * target + (1 - ALPHA_DOWN) * store.pApplied;
    }
    // PRIORITY 3: Surplus → slow increase (only every 5s)
    // Export > reserve, we can safely increase heater
    else {
      if (now - lastIncreaseMs >= INCREASE_INTERVAL_MS) {
        lastIncreaseMs = now;

        const avgGross = getAverageGross();
        const target = Math.max(0, Math.min(avgGross - EXPORT_RESERVE_W, P_MAX));

        if (target > store.pApplied) {
          store.pApplied =
            ALPHA_UP * target + (1 - ALPHA_UP) * store.pApplied;
        }
      }
    }

    // Rate limiter: max ±200W per tick
    const delta = store.pApplied - prevApplied;
    if (delta > MAX_STEP_W) store.pApplied = prevApplied + MAX_STEP_W;
    if (delta < -MAX_STEP_W) store.pApplied = prevApplied - MAX_STEP_W;

    // Clamp
    store.pApplied = Math.max(0, Math.min(store.pApplied, P_MAX));
    pTarget = store.pApplied;
  }

  store.lastRequestMs = now;

  // Duty cycle
  const dutyPct = Math.max(0, Math.min((store.pApplied / P_MAX) * 100, 100));

  // Update last report
  store.lastReport = {
    powerW: report.power_w,
    uptimeS: report.uptime_s,
    wifiRssi: report.wifi_rssi,
    freeHeap: report.free_heap,
    currentDutyPct: report.current_duty_pct,
    safeMode: report.safe_mode,
    receivedAt: now,
  };

  // Save history
  addHistoryEntry({
    timestamp: now,
    power_w: report.power_w,
    export_w: measuredExport,
    gross_export_w: grossExport,
    heater_w: store.pApplied,
    duty_pct: dutyPct,
    grid_import_w: gridImport,
  });

  // Persist to SQLite
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
