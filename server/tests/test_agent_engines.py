"""
Thesis IoT Server — Agent loop parity: SDK loop vs LangGraph

/agent/chat has two interchangeable loop implementations, picked by
settings.agent_engine ("sdk" = hand-written on the OpenAI SDK, "langgraph" =
StateGraph in app/agent/graph.py). Every test here runs against BOTH and
asserts the same observable behaviour: SSE events, what goes back to the
model, what gets persisted, and the safety properties from
test_agent_safety.py (confirm-before-delete, queued-vs-sent, per-call
exception isolation).

The model is faked at the HTTP transport, not by mocking SDK objects,
because the two engines use different client libraries (openai.AsyncOpenAI
vs langchain_openai.ChatOpenAI) that both end up there. A test that mocks
one library's objects silently lets the other one reach the real network.
Note the transport is httpx2's, not httpx's: openai>=3 ships its HTTP client
as the separate `httpx2` package and langchain-openai reuses it. Patching
plain httpx lets every request through to openrouter.ai (seen as a live 401
with the test key on the first attempt).
"""

import json
from unittest.mock import AsyncMock, patch

import httpx2
import pytest

import app.api.agent_routes  # noqa: F401
from app.agent.graph import MAX_AGENT_TURNS

ENGINES = ["sdk", "langgraph"]


@pytest.fixture(params=ENGINES)
def engine(request, monkeypatch):
    from app.config import settings
    monkeypatch.setattr(settings, "agent_engine", request.param)
    monkeypatch.setattr(settings, "openrouter_api_key", "test-key")
    return request.param


# ── Fake OpenRouter ─────────────────────────────────────────────────────────

def _tool_call(call_id: str, name: str, arguments: str = "{}") -> dict:
    return {"id": call_id, "type": "function", "function": {"name": name, "arguments": arguments}}


def _completion(content: str | None = None, tool_calls: list[dict] | None = None) -> dict:
    message = {"role": "assistant", "content": content}
    if tool_calls:
        message["tool_calls"] = tool_calls
    return {
        "id": "chatcmpl-test",
        "object": "chat.completion",
        "created": 0,
        "model": "test-model",
        "choices": [{
            "index": 0,
            "message": message,
            "finish_reason": "tool_calls" if tool_calls else "stop",
        }],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
    }


class FakeOpenRouter:
    """Serves scripted chat completions in order and records every request
    body, so tests can assert what the agent sent back to the model."""

    def __init__(self, *replies: dict):
        self.replies = list(replies)
        self.requests: list[dict] = []

    async def handle(self, transport, request):
        if not str(request.url).endswith("/chat/completions"):
            raise AssertionError(f"unexpected HTTP call in agent test: {request.url}")
        self.requests.append(json.loads(await request.aread()))
        reply = self.replies.pop(0) if len(self.replies) > 1 else self.replies[0]
        return httpx2.Response(200, json=reply, request=request)

    def tool_messages(self, request_index: int) -> list[dict]:
        return [m for m in self.requests[request_index]["messages"] if m["role"] == "tool"]


def _chat(client, fake: FakeOpenRouter, message: str, **body) -> list[dict]:  # noqa
    async def _handle(transport, request):
        return await fake.handle(transport, request)

    with patch.object(httpx2.AsyncHTTPTransport, "handle_async_request", _handle):
        resp = client.post("/api/agent/chat", json={"message": message, **body})
    assert resp.status_code == 200
    events = []
    for chunk in resp.text.split("\n\n"):
        chunk = chunk.strip()
        if chunk.startswith("data: "):
            events.append(json.loads(chunk[len("data: "):]))
    return events


def _types(events: list[dict]) -> list[str]:
    errors = [e["text"] for e in events if e["event"] == "error"]
    if errors:
        print("SSE error events:", errors)
    return [e["event"] for e in events]


# ── Basic loop shape ────────────────────────────────────────────────────────

def test_plain_reply_without_tools(client, engine):
    fake = FakeOpenRouter(_completion("The board is idle."))
    events = _chat(client, fake, "hello")

    assert _types(events) == ["thinking", "reply", "done"]
    assert events[1]["text"] == "The board is idle."
    assert len(fake.requests) == 1
    sent = fake.requests[0]
    assert sent["messages"][0]["role"] == "system"
    assert "WORLD STATE" in sent["messages"][0]["content"]
    assert sent["messages"][-1] == {"role": "user", "content": "hello"}
    assert {t["function"]["name"] for t in sent["tools"]} >= {"ping_board", "delete_schedule"}
    assert sent["temperature"] == 0.1


def test_tool_call_then_reply_feeds_result_back(client, engine):
    fake = FakeOpenRouter(
        _completion(tool_calls=[_tool_call("call_1", "ping_board")]),
        _completion("Pinged."),
    )
    with patch("app.api.agent_routes.send_board_command", return_value={"queued": False}):
        events = _chat(client, fake, "ping the board")

    assert _types(events) == ["thinking", "tool_call", "tool_result", "reply", "done"]
    assert events[1]["id"] == events[2]["id"] == "call_1"
    assert events[2]["success"] is True

    # Second model call sees: the assistant's tool call, then its result.
    assert len(fake.requests) == 2
    msgs = fake.requests[1]["messages"]
    assistant = [m for m in msgs if m["role"] == "assistant"][-1]
    assert assistant["tool_calls"][0]["id"] == "call_1"
    assert assistant["tool_calls"][0]["function"]["name"] == "ping_board"
    tool = fake.tool_messages(1)
    assert len(tool) == 1 and tool[0]["tool_call_id"] == "call_1"
    assert json.loads(tool[0]["content"])["success"] is True


def test_several_tool_calls_in_one_turn_all_run_in_order(client, engine):
    fake = FakeOpenRouter(
        _completion(tool_calls=[
            _tool_call("call_a", "ping_board"),
            _tool_call("call_b", "get_board_status"),
        ]),
        _completion("Both done."),
    )
    with patch("app.api.agent_routes.send_board_command", return_value={"queued": False}):
        events = _chat(client, fake, "ping and status")

    steps = [(e["event"], e["id"]) for e in events if e["event"] in ("tool_call", "tool_result")]
    assert steps == [
        ("tool_call", "call_a"), ("tool_result", "call_a"),
        ("tool_call", "call_b"), ("tool_result", "call_b"),
    ]
    assert [m["tool_call_id"] for m in fake.tool_messages(1)] == ["call_a", "call_b"]


def test_turn_cap_stops_a_model_that_never_stops_calling_tools(client, engine):
    fake = FakeOpenRouter(_completion(tool_calls=[_tool_call("call_x", "ping_board")]))
    with patch("app.api.agent_routes.send_board_command", return_value={"queued": False}):
        events = _chat(client, fake, "ping forever")

    assert len(fake.requests) == MAX_AGENT_TURNS
    assert _types(events).count("tool_result") == MAX_AGENT_TURNS
    assert _types(events)[-1] == "done"
    assert "error" not in _types(events)


def test_malformed_tool_arguments_run_with_empty_input(client, engine):
    """A tool call whose arguments are not JSON still runs (with {}) and still
    gets a matching tool message — the API rejects a follow-up request with
    an unanswered tool call."""
    fake = FakeOpenRouter(
        _completion(tool_calls=[_tool_call("call_bad", "ping_board", "{not json")]),
        _completion("ok"),
    )
    with patch("app.api.agent_routes.send_board_command", return_value={"queued": False}):
        events = _chat(client, fake, "ping")

    assert "error" not in _types(events)
    assert [m["tool_call_id"] for m in fake.tool_messages(1)] == ["call_bad"]


# ── Safety properties (same as test_agent_safety.py, through the loop) ──────

def test_tool_exception_is_isolated(client, engine):
    fake = FakeOpenRouter(
        _completion(tool_calls=[_tool_call("call_1", "ping_board")]),
        _completion("Something went wrong with the ping."),
    )
    with patch("app.api.agent_routes._execute_tool", new_callable=AsyncMock) as mock_exec:
        mock_exec.side_effect = RuntimeError("boom")
        events = _chat(client, fake, "ping the board")

    assert "error" not in _types(events)
    assert _types(events)[-1] == "done"
    result = next(e for e in events if e["event"] == "tool_result")
    assert result["success"] is False
    assert "ping_board" in result["summary"] and "boom" in result["summary"]
    fed_back = json.loads(fake.tool_messages(1)[0]["content"])
    assert fed_back["success"] is False


def test_delete_without_confirm_previews_and_keeps_the_schedule(client, engine):
    import asyncio

    from sqlalchemy import select

    from app.db import database
    from app.scheduler.models import Schedule
    from app.scheduler.service import create_schedule

    async def _make():
        async with database.async_session() as db:
            s = await create_schedule(db, name="Agent: night watch", description="", tasks=[
                {"time": "22:00", "action": "CAPTURE_IMAGE", "objective": "check the gate"},
            ])
            await db.commit()
            return s.id

    schedule_id = asyncio.run(_make())
    fake = FakeOpenRouter(
        _completion(tool_calls=[_tool_call(
            "call_del", "delete_schedule", json.dumps({"schedule_id": schedule_id}),
        )]),
        _completion("Delete schedule 'night watch'? This can't be undone."),
    )
    events = _chat(client, fake, "delete the night watch schedule")

    result = next(e for e in events if e["event"] == "tool_result")
    assert result["success"] is False
    fed_back = json.loads(fake.tool_messages(1)[0]["content"])
    assert fed_back["requires_confirmation"] is True
    assert "night watch" in fed_back["detail"]

    async def _still_there():
        async with database.async_session() as db:
            row = await db.execute(select(Schedule).where(Schedule.id == schedule_id))
            return row.scalar_one_or_none() is not None

    assert asyncio.run(_still_there())


def test_queued_command_is_reported_as_queued(client, engine):
    fake = FakeOpenRouter(
        _completion(tool_calls=[_tool_call("call_1", "ping_board")]),
        _completion("The board is asleep; the ping is queued."),
    )
    with patch(
        "app.api.agent_routes.send_board_command",
        return_value={"queued": True, "reason": "board dormant until 07:30"},
    ):
        events = _chat(client, fake, "ping the board")

    result = next(e for e in events if e["event"] == "tool_result")
    assert result["success"] is True
    assert "queued" in result["summary"].lower()
    assert "07:30" in json.loads(fake.tool_messages(1)[0]["content"])["detail"]


# ── Capture pipelines stream through, and persistence ───────────────────────

def test_capture_now_streams_pipeline_events_and_returns_its_reply(client, engine):
    from app.api.agent_routes import _sse_event

    async def fake_pipeline(tool_input, model_key, **kwargs):
        yield _sse_event("tool_call", {"id": "cap", "label": "Capturing..."})
        yield _sse_event("tool_result", {"id": "cap", "success": True, "summary": "Image received",
                                         "image_url": "/uploads/x.jpg"})
        yield _sse_event("reply", {"text": "A person is at the door."})

    fake = FakeOpenRouter(
        _completion(tool_calls=[_tool_call("call_cap", "capture_now")]),
        _completion("Someone is at the door."),
    )
    with patch("app.api.agent_routes._capture_pipeline", fake_pipeline):
        events = _chat(client, fake, "what do you see")

    assert _types(events) == ["thinking", "tool_call", "tool_result", "reply", "reply", "done"]
    assert events[2]["image_url"] == "/uploads/x.jpg"
    assert fake.tool_messages(1)[0]["content"] == "A person is at the door."


def test_persisted_message_holds_model_text_and_blocks(client, engine):
    from app.api.agent_routes import _sse_event

    session_id = client.post("/api/agent/sessions", json={"name": "t"}).json()["id"]

    async def fake_pipeline(tool_input, model_key, **kwargs):
        yield _sse_event("tool_call", {"id": "cap", "label": "Capturing..."})
        yield _sse_event("tool_result", {"id": "cap", "success": True, "summary": "ok"})
        yield _sse_event("reply", {"text": "pipeline analysis text"})

    fake = FakeOpenRouter(
        _completion("Taking a look.", tool_calls=[_tool_call("call_cap", "capture_now")]),
        _completion("All clear."),
    )
    with patch("app.api.agent_routes._capture_pipeline", fake_pipeline):
        _chat(client, fake, "look", sessionId=session_id)

    msgs = client.get(f"/api/agent/sessions/{session_id}/messages").json()
    assistant = [m for m in msgs if m["role"] == "assistant"][-1]
    # Only the model's own words are the message text; the pipeline's reply
    # lives in the blocks.
    assert assistant["content"] == "Taking a look.\n\nAll clear."
    texts = [b["text"] for b in assistant["blocks"] if b["type"] == "text"]
    assert texts == ["Taking a look.", "pipeline analysis text", "All clear."]
    assert any(b["type"] == "step" and b["status"] == "done" for b in assistant["blocks"])


# ── The two engines are indistinguishable from the outside ──────────────────

def test_both_engines_emit_identical_event_streams(client, monkeypatch):
    from app.config import settings

    monkeypatch.setattr(settings, "openrouter_api_key", "test-key")
    script = [
        _completion("Checking.", tool_calls=[
            _tool_call("call_1", "ping_board"),
            _tool_call("call_2", "get_board_status"),
        ]),
        _completion("Board answered."),
    ]
    streams, bodies = {}, {}
    for name in ENGINES:
        monkeypatch.setattr(settings, "agent_engine", name)
        fake = FakeOpenRouter(*script)
        with patch("app.api.agent_routes.send_board_command", return_value={"queued": False}):
            streams[name] = _chat(client, fake, "ping and status")
        bodies[name] = fake.requests

    assert streams["sdk"] == streams["langgraph"]

    # The model gets the same conversation from both — compared on what
    # matters to it (roles, text, tool calls, tool results).
    def _shape(req):
        out = []
        for m in req["messages"]:
            calls = [(c["id"], c["function"]["name"]) for c in m.get("tool_calls") or []]
            out.append((m["role"], m.get("content") or "", calls, m.get("tool_call_id")))
        return out

    assert [_shape(r) for r in bodies["sdk"]] == [_shape(r) for r in bodies["langgraph"]]
