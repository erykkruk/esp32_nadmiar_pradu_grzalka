import { NextRequest, NextResponse } from "next/server";

const DASHBOARD_PASSWORD = process.env.DASHBOARD_PASSWORD || "";

export async function POST(req: NextRequest) {
  const body = await req.json();

  if (!DASHBOARD_PASSWORD) {
    // No password configured - allow access
    const res = NextResponse.json({ ok: true });
    res.cookies.set("auth", "open", {
      httpOnly: true,
      secure: true,
      sameSite: "strict",
      maxAge: 60 * 60 * 24 * 30, // 30 days
    });
    return res;
  }

  if (body.password === DASHBOARD_PASSWORD) {
    const token = Buffer.from(DASHBOARD_PASSWORD).toString("base64");
    const res = NextResponse.json({ ok: true });
    res.cookies.set("auth", token, {
      httpOnly: true,
      secure: true,
      sameSite: "strict",
      maxAge: 60 * 60 * 24 * 30, // 30 days
    });
    return res;
  }

  return NextResponse.json({ error: "wrong password" }, { status: 401 });
}
