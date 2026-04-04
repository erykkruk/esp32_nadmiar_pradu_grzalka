import { NextRequest, NextResponse } from "next/server";
import { processReport } from "@/lib/heater-logic";
import type { EspReport } from "@/lib/store";

const API_KEY = process.env.API_KEY || "change-me-to-a-random-secret";

export async function POST(req: NextRequest) {
  const auth = req.headers.get("authorization");
  if (auth !== `Bearer ${API_KEY}`) {
    return NextResponse.json({ error: "unauthorized" }, { status: 401 });
  }

  const body = (await req.json()) as Partial<EspReport>;

  const report: EspReport = {
    power_w: body.power_w ?? 0,
    uptime_s: body.uptime_s ?? 0,
    wifi_rssi: body.wifi_rssi ?? 0,
    free_heap: body.free_heap ?? 0,
    current_duty_pct: body.current_duty_pct ?? 0,
    safe_mode: body.safe_mode ?? false,
    seconds_since_last_response: body.seconds_since_last_response ?? 0,
  };

  const result = processReport(report);

  return NextResponse.json(result);
}
