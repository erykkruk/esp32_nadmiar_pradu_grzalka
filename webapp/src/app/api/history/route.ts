import { NextRequest, NextResponse } from "next/server";
import { getHistory } from "@/lib/db";

export const dynamic = "force-dynamic";

// Resolution per range (seconds between averaged points)
const RANGE_CONFIG: Record<string, { durationMs: number; resolutionS?: number }> = {
  "10m": { durationMs: 10 * 60_000 },                    // raw data (~200ms points)
  "1h":  { durationMs: 3600_000,       resolutionS: 30 },  // 1 point per 30s
  "6h":  { durationMs: 6 * 3600_000,   resolutionS: 300 }, // 1 point per 5min
  "24h": { durationMs: 24 * 3600_000,  resolutionS: 1200 },// 1 point per 20min
  "7d":  { durationMs: 7 * 86400_000,  resolutionS: 3600 },// 1 point per 1h
  "30d": { durationMs: 30 * 86400_000, resolutionS: 14400 },// 1 point per 4h
};

export async function GET(req: NextRequest) {
  const sp = req.nextUrl.searchParams;
  const range = sp.get("range") || "1h";
  const now = Date.now();

  const config = RANGE_CONFIG[range];
  const from = config
    ? now - config.durationMs
    : parseInt(sp.get("from") || String(now - 3600_000), 10);
  const to = parseInt(sp.get("to") || String(now), 10);

  const data = getHistory({
    from,
    to,
    resolution: config?.resolutionS,
    limit: 10000,
  });

  return NextResponse.json({ from, to, count: data.length, data });
}
