import { NextResponse } from "next/server";
import { getLatestReading } from "@/lib/db";

export const dynamic = "force-dynamic";

export async function GET() {
  const reading = getLatestReading();
  if (!reading) {
    return NextResponse.json({ error: "no data yet" }, { status: 404 });
  }
  return NextResponse.json(reading);
}
