import { NextRequest, NextResponse } from "next/server";
import { getStore } from "@/lib/store";

export async function POST(req: NextRequest) {
  const body = await req.json();
  const store = getStore();

  if (body.mode === "auto" || body.mode === "manual") {
    store.mode = body.mode;
  }

  if (typeof body.duty === "number") {
    store.manualDuty = Math.max(0, Math.min(body.duty, 100));
  }

  return NextResponse.json({
    mode: store.mode,
    manual_duty: store.manualDuty,
    ok: true,
  });
}
