"""
Thesis IoT Server — Owner login

Hesperus has one human user, the owner. With the physical board gone
(2026-11), the phone and the laptop are both just the owner's browser, so
the question every request answers is "is this the owner?", not "is this
my board?".

  - POST /api/auth/login {password} checks settings.owner_password and sets
    an HttpOnly session cookie (signed, 30 days).
  - OwnerAuthMiddleware rejects every request without a valid cookie (401),
    except the paths in PUBLIC_PATHS / PUBLIC_PREFIXES below.
  - verify_board_token (app/api/routes.py) also accepts the cookie, so the
    phone camera page can upload without a device token.

Enforced here in FastAPI, not only in the Next.js dashboard: nginx sends
/api/* straight to this server and port 8000 is open to the internet (it was
the board's way in), so a dashboard-only login would be bypassed.

When owner_password is empty, auth is OFF and everything is open, same
dev-mode convention as firmware_upload_token. Production must set
OWNER_PASSWORD.
"""

import hashlib
import hmac
import time

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import JSONResponse

from app.config import settings

COOKIE_NAME = "hesperus_session"
SESSION_DAYS = 30

# Reachable without logging in.
PUBLIC_PATHS = {
    "/health",
    "/api/auth/login",
    "/api/auth/logout",
    "/api/auth/me",
    "/api/time",  # board clock sync, no data
}
PUBLIC_PREFIXES = (
    # CI uploads firmware with X-Firmware-Token; each firmware route keeps its
    # own token check (app/api/firmware_routes.py).
    "/api/firmware/",
)
# Board ingress: these routes carry Depends(verify_board_token), which accepts
# either the board's X-Upload-Token or the owner cookie.
BOARD_PATHS = {
    "/api/upload",
    "/api/upload/chunk",
    "/api/upload/resume",
    "/api/upload/complete",
    "/api/erase-wifi",
}


def auth_enabled() -> bool:
    return bool(settings.owner_password)


def _key() -> bytes:
    # Derived from the password, so changing OWNER_PASSWORD logs out every
    # existing session.
    return hashlib.sha256(b"hesperus-session:" + settings.owner_password.encode()).digest()


def make_session_token(now: float | None = None) -> str:
    expires = int((now or time.time()) + SESSION_DAYS * 86400)
    sig = hmac.new(_key(), str(expires).encode(), hashlib.sha256).hexdigest()
    return f"{expires}.{sig}"


def valid_session_token(token: str | None, now: float | None = None) -> bool:
    if not token or "." not in token:
        return False
    expires_s, sig = token.split(".", 1)
    if not expires_s.isdigit():
        return False
    expected = hmac.new(_key(), expires_s.encode(), hashlib.sha256).hexdigest()
    return hmac.compare_digest(sig, expected) and int(expires_s) > (now or time.time())


def is_owner(request: Request) -> bool:
    """True when auth is off, or the request carries a valid owner cookie."""
    return not auth_enabled() or valid_session_token(request.cookies.get(COOKIE_NAME))


def _is_public(path: str) -> bool:
    return path in PUBLIC_PATHS or path.startswith(PUBLIC_PREFIXES) or path in BOARD_PATHS


class OwnerAuthMiddleware:
    """Pure ASGI middleware (not BaseHTTPMiddleware): the agent chat is an
    SSE stream that relies on request.is_disconnected(), which
    BaseHTTPMiddleware's request wrapping is known to interfere with."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http" or not auth_enabled() or _is_public(scope["path"]):
            await self.app(scope, receive, send)
            return
        request = Request(scope)
        if valid_session_token(request.cookies.get(COOKIE_NAME)):
            await self.app(scope, receive, send)
            return
        response = JSONResponse({"detail": "Login required"}, status_code=401)
        await response(scope, receive, send)


# ── Login routes ────────────────────────────────────────────

router = APIRouter(prefix="/auth", tags=["Auth"])

# Failed logins per client IP: (count, window start). Slows down password
# guessing without a database table; resets on server restart.
_failures: dict[str, tuple[int, float]] = {}
MAX_FAILURES = 10
FAILURE_WINDOW_S = 15 * 60


def _client_ip(request: Request) -> str:
    # nginx sets X-Real-IP; direct hits on :8000 fall back to the socket peer.
    return request.headers.get("x-real-ip") or (request.client.host if request.client else "?")


@router.post("/login")
async def login(request: Request):
    if not auth_enabled():
        # Still set a cookie: the dashboard's proxy.ts only lets a browser
        # past /login once the cookie exists. With auth off its value is
        # never checked.
        response = JSONResponse({"ok": True, "auth": False})
        response.set_cookie(COOKIE_NAME, "dev", httponly=True, samesite="lax", path="/")
        return response

    ip = _client_ip(request)
    count, since = _failures.get(ip, (0, time.time()))
    if time.time() - since > FAILURE_WINDOW_S:
        count, since = 0, time.time()
    if count >= MAX_FAILURES:
        raise HTTPException(status_code=429, detail="Too many attempts, try again in 15 minutes")

    try:
        body = await request.json()
    except ValueError:
        body = {}
    password = body.get("password", "") if isinstance(body, dict) else ""
    if not isinstance(password, str) or not hmac.compare_digest(password, settings.owner_password):
        _failures[ip] = (count + 1, since)
        raise HTTPException(status_code=401, detail="Wrong password")

    _failures.pop(ip, None)
    response = JSONResponse({"ok": True, "auth": True})
    response.set_cookie(
        COOKIE_NAME,
        make_session_token(),
        max_age=SESSION_DAYS * 86400,
        httponly=True,
        # nginx (the only thing that sets X-Real-IP) is HTTPS-only, so a
        # request that came through it was HTTPS even though nginx talks
        # plain HTTP to this server and sends no X-Forwarded-Proto.
        secure=request.url.scheme == "https" or "x-real-ip" in request.headers,
        samesite="lax",
        path="/",
    )
    return response


@router.post("/logout")
async def logout():
    response = JSONResponse({"ok": True})
    response.delete_cookie(COOKIE_NAME, path="/")
    return response


@router.get("/me")
async def me(request: Request):
    return {"auth": auth_enabled(), "owner": is_owner(request)}
