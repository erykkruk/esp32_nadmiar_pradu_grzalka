import { NextRequest, NextResponse } from "next/server";
import { getHistory } from "@/lib/db";

export const dynamic = "force-dynamic";

// Resolution per range (seconds between averaged points)
// Target: ~200-400 data points per range for smooth Recharts curves
const RANGE_CONFIG: Record<string, { durationMs: number; resolutionS?: number }> = {
  "10m": { durationMs: 10 * 60_000 },                      // raw data (~600 points)
  "1h":  { durationMs: 3600_000,       resolutionS: 10 },   // 1pt/10s → 360 points
  "6h":  { durationMs: 6 * 3600_000,   resolutionS: 60 },   // 1pt/1min → 360 points
  "24h": { durationMs: 24 * 3600_000,  resolutionS: 300 },  // 1pt/5min → 288 points
  "7d":  { durationMs: 7 * 86400_000,  resolutionS: 1800 }, // 1pt/30min → 336 points
  "30d": { durationMs: 30 * 86400_000, resolutionS: 7200 }, // 1pt/2h → 360 points
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
