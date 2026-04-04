import { NextRequest, NextResponse } from "next/server";
import { startDemo, stopDemo, getDemoParams } from "@/lib/demo";

export const dynamic = "force-dynamic";

// GET - demo status
export async function GET() {
  return NextResponse.json(getDemoParams());
}

// POST - start demo
export async function POST(req: NextRequest) {
  const body = await req.json().catch(() => ({}));

  startDemo({
    solar: body.solar,
    house: body.house,
    noise: body.noise,
    spike: body.spike,
  });

  return NextResponse.json({ ok: true, ...getDemoParams() });
}

// DELETE - stop demo
export async function DELETE() {
  stopDemo();
  return NextResponse.json({ ok: true, running: false });
}
