import { NextResponse } from "next/server";
import { getStore } from "@/lib/store";

export const dynamic = "force-dynamic";

export async function GET() {
  const store = getStore();

  const measuredExport =
    store.lastReport && store.lastReport.powerW < 0
      ? -store.lastReport.powerW
      : 0;
  const gridImport =
    store.lastReport && store.lastReport.powerW > 0
      ? store.lastReport.powerW
      : 0;

  return NextResponse.json({
    mode: store.mode,
    p_applied: Math.round(store.pApplied * 10) / 10,
    p_max: store.pMax,
    p_target:
      store.mode === "manual"
        ? (store.manualDuty / 100) * store.pMax
        : store.pApplied,
    duty_pct: Math.round((store.pApplied / store.pMax) * 1000) / 10,
    manual_duty: store.manualDuty,
    export_w: Math.round(measuredExport * 10) / 10,
    grid_import_w: Math.round(gridImport * 10) / 10,
    heater_w: Math.round(store.pApplied * 10) / 10,
    gross_export_w: Math.round((measuredExport + store.pApplied) * 10) / 10,
    esp_online: store.lastReport
      ? Date.now() - store.lastReport.receivedAt < 30000
      : false,
    esp_safe_mode: store.lastReport?.safeMode ?? false,
    esp_rssi: store.lastReport?.wifiRssi ?? 0,
    esp_uptime_s: store.lastReport?.uptimeS ?? 0,
    esp_free_heap: store.lastReport?.freeHeap ?? 0,
    last_report_at: store.lastReport?.receivedAt ?? null,
  });
}
