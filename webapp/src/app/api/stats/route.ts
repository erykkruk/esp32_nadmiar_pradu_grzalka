import { NextResponse } from "next/server";
import { getDbStats } from "@/lib/db";

export const dynamic = "force-dynamic";

export async function GET() {
  return NextResponse.json(getDbStats());
}
