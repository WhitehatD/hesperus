// Owner login gate for the dashboard pages (Next.js 16 "proxy", formerly
// middleware). Sends anyone without the session cookie to /login.
//
// This only checks that the cookie EXISTS: the real check (signature,
// expiry) is FastAPI's (server/app/auth.py), which every data request goes
// through. A forged cookie gets past this redirect and then sees nothing but
// 401s. The redirect is here so a logged-out visitor lands on the login form
// instead of a dashboard full of errors.
//
// When the server has no owner password configured (local dev), /login says
// so and sends the user straight back.

import { type NextRequest, NextResponse } from "next/server";

const COOKIE_NAME = "hesperus_session";

export function proxy(request: NextRequest) {
	if (request.cookies.has(COOKIE_NAME)) return NextResponse.next();
	const url = request.nextUrl.clone();
	url.pathname = "/login";
	url.search = `?next=${encodeURIComponent(request.nextUrl.pathname + request.nextUrl.search)}`;
	return NextResponse.redirect(url);
}

export const config = {
	// Everything except the login page itself, API routes (FastAPI and the
	// agent proxy answer 401 on their own), Next internals and static files.
	matcher: [
		"/((?!login|api|mqtt|_next|favicon.ico|.*\\.(?:svg|png|jpg|ico|css|js)$).*)",
	],
};
