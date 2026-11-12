"""
Thesis IoT Server — Owner login (app/auth.py)

With settings.owner_password set, every route needs the owner session
cookie except health, the login routes themselves, firmware (own token) and
board ingress (board token OR owner cookie). With it empty, nothing changes:
that is how every other test in this suite runs.
"""

import pytest

from app import auth
from app.config import settings

PASSWORD = "correct horse battery staple"


@pytest.fixture
def owner(monkeypatch):
    monkeypatch.setattr(settings, "owner_password", PASSWORD)
    monkeypatch.setattr(settings, "firmware_upload_token", "board-secret")
    auth._failures.clear()
    yield
    auth._failures.clear()


def _login(client, password=PASSWORD):
    return client.post("/api/auth/login", json={"password": password})


def test_auth_off_when_no_password_is_configured(client):
    assert client.get("/api/agent/sessions").status_code == 200
    assert client.get("/api/auth/me").json() == {"auth": False, "owner": True}


def test_protected_routes_need_login(client, owner):
    for method, path in [
        ("get", "/api/agent/sessions"),
        ("post", "/api/agent/chat"),
        ("get", "/api/images"),
        ("get", "/api/schedules"),
        ("get", "/api/board/state"),
        ("post", "/api/ping"),
    ]:
        r = getattr(client, method)(path)
        assert r.status_code == 401, (method, path, r.status_code)


def test_public_routes_stay_open(client, owner):
    assert client.get("/health").status_code == 200
    assert client.get("/api/time").status_code == 200
    assert client.get("/api/auth/me").json() == {"auth": True, "owner": False}
    # Firmware keeps its own token check: reaches the route, which says 403.
    assert client.get("/api/firmware/version").status_code == 403


def test_wrong_password_is_rejected_and_sets_no_cookie(client, owner):
    r = _login(client, "nope")
    assert r.status_code == 401
    assert auth.COOKIE_NAME not in r.cookies
    assert client.get("/api/agent/sessions").status_code == 401


def test_login_gives_a_cookie_that_opens_the_api(client, owner):
    r = _login(client)
    assert r.status_code == 200
    assert auth.COOKIE_NAME in client.cookies
    set_cookie = r.headers["set-cookie"].lower()
    assert "httponly" in set_cookie and "samesite=lax" in set_cookie

    assert client.get("/api/agent/sessions").status_code == 200
    assert client.get("/api/auth/me").json() == {"auth": True, "owner": True}


def test_logout_closes_it_again(client, owner):
    _login(client)
    client.post("/api/auth/logout")
    assert client.get("/api/agent/sessions").status_code == 401


def test_forged_and_expired_tokens_are_rejected(client, owner):
    client.cookies.set(auth.COOKIE_NAME, "99999999999.deadbeef")
    assert client.get("/api/agent/sessions").status_code == 401

    expired = auth.make_session_token(now=1_000_000)  # 1970 + 30 days
    client.cookies.set(auth.COOKIE_NAME, expired)
    assert client.get("/api/agent/sessions").status_code == 401


def test_changing_the_password_logs_everyone_out(client, owner, monkeypatch):
    _login(client)
    monkeypatch.setattr(settings, "owner_password", "a new password")
    assert client.get("/api/agent/sessions").status_code == 401


def test_too_many_wrong_passwords_locks_login(client, owner):
    for _ in range(auth.MAX_FAILURES):
        assert _login(client, "nope").status_code == 401
    assert _login(client).status_code == 429  # even the right one, for now


def test_upload_accepts_owner_cookie_instead_of_board_token(client, owner):
    jpeg = b"\xff\xd8\xff\xe0" + b"0" * 64 + b"\xff\xd9"
    files = {"file": ("shot.jpg", jpeg, "image/jpeg")}

    r = client.post("/api/upload?task_id=5", files=files)
    assert r.status_code == 403  # no token, no cookie

    r = client.post("/api/upload?task_id=5", files=files, headers={"X-Upload-Token": "board-secret"})
    assert r.status_code == 200  # board token still works

    _login(client)
    r = client.post("/api/upload?task_id=6", files=files)
    assert r.status_code == 200  # owner cookie works too


def test_upload_does_not_accept_a_cookie_when_auth_is_off(client, monkeypatch):
    """With no owner password, is_owner() is always true; it must not turn
    into a bypass of the board token."""
    monkeypatch.setattr(settings, "firmware_upload_token", "board-secret")
    files = {"file": ("shot.jpg", b"\xff\xd8\xff\xd9", "image/jpeg")}
    assert client.post("/api/upload?task_id=7", files=files).status_code == 403


@pytest.mark.parametrize("framework", ["sdk", "langchain"])
def test_logged_in_agent_chat_streams_through_the_middleware(client, owner, monkeypatch, framework):
    """The chat is an SSE stream; the auth middleware must pass it through
    whole (every event, in order) once the owner is logged in."""
    from test_agent_engines import FakeOpenRouter, _chat, _completion, _tool_call
    from unittest.mock import patch

    monkeypatch.setattr(settings, "openrouter_api_key", "test-key")
    monkeypatch.setattr(settings, "llm_framework", framework)
    _login(client)
    fake = FakeOpenRouter(
        _completion(tool_calls=[_tool_call("call_1", "ping_board")]),
        _completion("Pinged."),
    )
    with patch("app.api.agent_routes.send_board_command", return_value={"queued": False}):
        events = _chat(client, fake, "ping the board")
    assert [e["event"] for e in events] == ["thinking", "tool_call", "tool_result", "reply", "done"]


# ── Virtual board status (app/api/camera_routes.py) ─────────────────────────

def test_camera_status_needs_login(client, owner):
    assert client.post("/api/camera/status", json={"status": "online"}).status_code == 401


def test_camera_status_is_republished_as_a_board_status(client, owner):
    import json as _json

    _login(client)
    r = client.post("/api/camera/status", json={"status": "ping_received"})
    assert r.status_code == 200
    topic, payload = client._mock_mqtt.publish.call_args.args
    assert topic == settings.mqtt_topic_status
    assert _json.loads(payload) == {"status": "ping_received", "firmware": "phone-camera"}


def test_camera_status_refuses_unknown_values(client, owner):
    _login(client)
    r = client.post("/api/camera/status", json={"status": "deep_dormant"})
    assert r.status_code == 422
