import { NextRequest, NextResponse } from "next/server";
import { getStore } from "@/lib/store";

const P_MAX_MIN = 100;
const P_MAX_MAX = 10000;

export async function GET() {
  const store = getStore();

  return NextResponse.json({
    p_max: store.pMax,
  });
}

export async function POST(req: NextRequest) {
  const body = await req.json();
  const store = getStore();

  if (typeof body.p_max === "number") {
    store.pMax = Math.max(P_MAX_MIN, Math.min(body.p_max, P_MAX_MAX));
  }

  return NextResponse.json({
    p_max: store.pMax,
    ok: true,
  });
}
