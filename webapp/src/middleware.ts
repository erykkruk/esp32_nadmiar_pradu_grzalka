import { NextRequest, NextResponse } from "next/server";

export function middleware(req: NextRequest) {
  const { pathname } = req.nextUrl;

  // Allow ESP32 endpoint and auth endpoint without password
  if (
    pathname.startsWith("/api/esp/") ||
    pathname.startsWith("/api/auth")
  ) {
    return NextResponse.next();
  }

  const password = process.env.DASHBOARD_PASSWORD || "";

  // No password configured - allow all
  if (!password) {
    return NextResponse.next();
  }

  // Check auth cookie
  const authCookie = req.cookies.get("auth")?.value;
  const expectedToken = Buffer.from(password).toString("base64");

  if (authCookie === expectedToken || authCookie === "open") {
    return NextResponse.next();
  }

  // API routes return 401
  if (pathname.startsWith("/api/")) {
    return NextResponse.json({ error: "unauthorized" }, { status: 401 });
  }

  // Dashboard pages - let them load (client-side will show login form)
  return NextResponse.next();
}

export const config = {
  matcher: ["/((?!_next/static|_next/image|favicon.ico).*)"],
};
