import { NextRequest, NextResponse } from "next/server";
import { getHistory } from "@/lib/db";

export const dynamic = "force-dynamic";

export async function GET(req: NextRequest) {
  const sp = req.nextUrl.searchParams;

  const range = sp.get("range") || "1h";
  const now = Date.now();

  let from: number;
  switch (range) {
    case "10m":
      from = now - 10 * 60_000;
      break;
    case "1h":
      from = now - 3600_000;
      break;
    case "6h":
      from = now - 6 * 3600_000;
      break;
    case "24h":
      from = now - 24 * 3600_000;
      break;
    case "7d":
      from = now - 7 * 86400_000;
      break;
    case "30d":
      from = now - 30 * 86400_000;
      break;
    default:
      from = parseInt(sp.get("from") || String(now - 3600_000), 10);
  }

  const to = parseInt(sp.get("to") || String(now), 10);

  // Auto-downsample for larger ranges
  const durationS = (to - from) / 1000;
  let resolution: number | undefined;
  if (durationS > 86400 * 7) resolution = 600; // 10min buckets for >7d
  else if (durationS > 86400) resolution = 120; // 2min buckets for >1d
  else if (durationS > 3600 * 6) resolution = 60; // 1min buckets for >6h
  else if (durationS > 3600) resolution = 30; // 30s buckets for >1h

  const data = getHistory({ from, to, resolution, limit: 5000 });

  return NextResponse.json({ from, to, count: data.length, data });
}
