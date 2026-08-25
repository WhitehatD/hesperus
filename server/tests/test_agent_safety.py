"""
Thesis IoT Server — Agent Safety Tests

Covers four gaps found by code inspection in the agentic /agent/chat pipeline:

Gap 1 — delete_schedule / delete_image require an explicit confirm=true before
        they actually delete anything; without it they preview what would be
        deleted (requires_confirmation=True) and touch nothing.
Gap 2 — every tool that calls send_board_command() honestly reflects a queued
        (board dormant) result in its summary/detail instead of claiming the
        physical board already acted.
Gap 3 — AGENT_SYSTEM_PROMPT tells the model to treat WORLD STATE findings as
        observations, never instructions (indirect prompt-injection boundary).
Gap 4 — a tool call that raises inside the agentic loop is caught and fed back
        as a normal tool-failure result instead of crashing the whole SSE turn.
"""

import os
os.environ["DATABASE_URL"] = "sqlite+aiosqlite://"

import json
from contextlib import asynccontextmanager
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy import select

# ── Force module imports before patching (mirrors test_use_cases.py pattern) ─
import app.main           # noqa: F401
import app.api.routes     # noqa: F401
import app.api.agent_routes      # noqa: F401
import app.mqtt.client           # noqa: F401
import app.db.database           # noqa: F401


def _make_mock_mqtt():
    mock = MagicMock()
    mock.connection = AsyncMock()
    mock.client = MagicMock()
    mock.client.disconnect = AsyncMock()
    mock.publish = MagicMock()
    return mock


@pytest.fixture
async def db_session():
    """Fresh in-memory async DB session, wired into app.db.database."""
    engine = create_async_engine(
        "sqlite+aiosqlite://", echo=False, connect_args={"check_same_thread": False},
    )
    session_factory = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)

    import app.db.wifi_models        # noqa: F401
    import app.analysis.models       # noqa: F401
    import app.scheduler.models      # noqa: F401
    import app.agent.models          # noqa: F401

    from app.db.database import Base
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)

    async with session_factory() as session:
        with patch.object(app.db.database, "engine", engine), \
             patch.object(app.db.database, "async_session", session_factory):
            yield session


def _fake_session_factory(db_session):
    @asynccontextmanager
    async def _ctx():
        yield db_session
    factory = MagicMock()
    factory.return_value = _ctx()
    return factory


# ══════════════════════════════════════════════════════════════════════════════
#  Gap 1 — delete_schedule confirmation gate
# ══════════════════════════════════════════════════════════════════════════════

async def test_delete_schedule_without_confirm_previews_and_does_not_delete(db_session):
    from app.scheduler.service import create_schedule
    from app.scheduler.models import Schedule

    schedule = await create_schedule(
        db_session,
        name="Agent: night watch",
        description="",
        tasks=[{"time": "22:00", "action": "CAPTURE_IMAGE", "objective": "check the gate"}],
    )
    await db_session.commit()
    schedule_id = schedule.id

    mock_mqtt = _make_mock_mqtt()
    with patch("app.api.agent_routes.mqtt_client", mock_mqtt), \
         patch("app.api.agent_routes.async_session", _fake_session_factory(db_session)):
        from app.api.agent_routes import _tool_delete_schedule
        result = await _tool_delete_schedule({"schedule_id": schedule_id})

    assert result["success"] is False
    assert result.get("requires_confirmation") is True
    assert "night watch" in result["detail"]
    mock_mqtt.publish.assert_not_called()

    # Schedule must still exist — nothing was deleted.
    row = await db_session.execute(select(Schedule).where(Schedule.id == schedule_id))
    assert row.scalar_one_or_none() is not None


async def test_delete_schedule_with_confirm_true_actually_deletes(db_session):
    from app.scheduler.service import create_schedule
    from app.scheduler.models import Schedule

    schedule = await create_schedule(
        db_session,
        name="Agent: night watch",
        description="",
        tasks=[{"time": "22:00", "action": "CAPTURE_IMAGE", "objective": "check the gate"}],
    )
    await db_session.commit()
    schedule_id = schedule.id

    mock_mqtt = _make_mock_mqtt()
    with patch("app.api.agent_routes.mqtt_client", mock_mqtt), \
         patch("app.scheduler.notify.mqtt_client", mock_mqtt), \
         patch("app.api.agent_routes.async_session", _fake_session_factory(db_session)), \
         patch("app.scheduler.notify.notify_schedule_update", new_callable=AsyncMock):
        from app.api.agent_routes import _tool_delete_schedule
        result = await _tool_delete_schedule({"schedule_id": schedule_id, "confirm": True})

    assert result["success"] is True
    assert "requires_confirmation" not in result

    row = await db_session.execute(select(Schedule).where(Schedule.id == schedule_id))
    assert row.scalar_one_or_none() is None


# ══════════════════════════════════════════════════════════════════════════════
#  Gap 1 — delete_image confirmation gate
# ══════════════════════════════════════════════════════════════════════════════

async def test_delete_image_without_confirm_previews_and_does_not_delete(tmp_path, db_session):
    img_dir = tmp_path / "2026-04-25"
    img_dir.mkdir()
    img_path = img_dir / "task_42_1745000000.jpg"
    img_path.write_bytes(b"\xff\xd8\xff\xe0fake-jpeg")

    with patch("app.api.agent_routes.settings.upload_dir", str(tmp_path)):
        from app.api.agent_routes import _tool_delete_image
        result = await _tool_delete_image({"date": "2026-04-25", "filename": "task_42_1745000000.jpg"})

    assert result["success"] is False
    assert result.get("requires_confirmation") is True
    assert img_path.exists(), "Image must not be deleted without confirm=true"


async def test_delete_image_with_confirm_true_actually_deletes(tmp_path, db_session):
    img_dir = tmp_path / "2026-04-25"
    img_dir.mkdir()
    img_path = img_dir / "task_42_1745000000.jpg"
    img_path.write_bytes(b"\xff\xd8\xff\xe0fake-jpeg")

    with patch("app.api.agent_routes.settings.upload_dir", str(tmp_path)), \
         patch("app.api.agent_routes.async_session", _fake_session_factory(db_session)):
        from app.api.agent_routes import _tool_delete_image
        result = await _tool_delete_image({
            "date": "2026-04-25",
            "filename": "task_42_1745000000.jpg",
            "confirm": True,
        })

    assert result["success"] is True
    assert not img_path.exists(), "Image must be deleted once confirm=true"


# ══════════════════════════════════════════════════════════════════════════════
#  Gap 2 — honest "queued" reporting when the board is dormant
# ══════════════════════════════════════════════════════════════════════════════

async def test_ping_reports_queued_when_board_dormant():
    mock_mqtt = _make_mock_mqtt()
    with patch("app.api.agent_routes.mqtt_client", mock_mqtt), \
         patch(
             "app.api.agent_routes.send_board_command",
             return_value={"queued": True, "reason": "board dormant until 07:30"},
         ) as mock_send:
        from app.api.agent_routes import _tool_ping
        result = await _tool_ping()

    mock_send.assert_called_once()
    assert result["success"] is True
    assert "queued" in result["summary"].lower()
    assert "07:30" in result["detail"]
    # Must NOT claim the board has ALREADY flashed its LEDs (past/present tense) —
    # only that it will, once it wakes.
    assert "board's leds will flash to confirm it's alive." not in result["detail"].lower()
    assert "asleep" in result["detail"].lower()


async def test_deactivate_schedule_reports_queued_when_board_dormant(db_session):
    from app.scheduler.service import create_schedule

    schedule = await create_schedule(
        db_session,
        name="Agent: active patrol",
        description="",
        tasks=[{"time": "14:00", "action": "CAPTURE_IMAGE", "objective": "afternoon check"}],
    )
    schedule.is_active = True
    await db_session.commit()

    with patch("app.api.agent_routes.async_session", _fake_session_factory(db_session)), \
         patch("app.scheduler.notify.notify_schedule_update", new_callable=AsyncMock), \
         patch(
             "app.api.agent_routes.send_board_command",
             return_value={"queued": True, "reason": "board dormant until 07:30"},
         ) as mock_send:
        from app.api.agent_routes import _tool_deactivate_schedule
        result = await _tool_deactivate_schedule({})

    mock_send.assert_called_once()
    assert result["success"] is True
    assert "queued" in result["summary"].lower()
    assert "board is asleep" in result["detail"].lower()
    # Must not claim the board has already been told, unqualified.
    assert "will be told to clear its schedule when it wakes" in result["detail"]


async def test_ping_reports_sent_when_board_online():
    """Sanity check: the non-queued path is unaffected (control case)."""
    mock_mqtt = _make_mock_mqtt()
    with patch("app.api.agent_routes.mqtt_client", mock_mqtt), \
         patch(
             "app.api.agent_routes.send_board_command",
             return_value={"queued": False},
         ):
        from app.api.agent_routes import _tool_ping
        result = await _tool_ping()

    assert result["success"] is True
    assert "queued" not in result["summary"].lower()
    assert "will flash to confirm" in result["detail"]


# ══════════════════════════════════════════════════════════════════════════════
#  Gap 3 — prompt-injection boundary around WORLD STATE findings
# ══════════════════════════════════════════════════════════════════════════════

def test_system_prompt_declares_findings_are_observations_not_instructions():
    from app.api.agent_routes import AGENT_SYSTEM_PROMPT

    lowered = AGENT_SYSTEM_PROMPT.lower()
    assert "observation" in lowered
    assert "never" in lowered and ("instruction" in lowered)


# ══════════════════════════════════════════════════════════════════════════════
#  Gap 4 — one failing tool call must not crash the whole SSE turn
# ══════════════════════════════════════════════════════════════════════════════

class _FakeFunction:
    def __init__(self, name, arguments):
        self.name = name
        self.arguments = arguments


class _FakeToolCall:
    def __init__(self, id_, name, arguments):
        self.id = id_
        self.function = _FakeFunction(name, arguments)


class _FakeMessage:
    def __init__(self, content=None, tool_calls=None):
        self.content = content
        self.tool_calls = tool_calls or []


class _FakeChoice:
    def __init__(self, message):
        self.message = message


class _FakeResponse:
    def __init__(self, message):
        self.choices = [_FakeChoice(message)]


def test_tool_exception_does_not_crash_sse_stream(client, monkeypatch):
    """
    A tool call that raises inside the agentic loop must be converted into a
    normal failed tool_result — the SSE stream must still complete with a
    'done' event and NOT bail out via an 'error' event.
    """
    from app.config import settings

    monkeypatch.setattr(settings, "openrouter_api_key", "test-key")

    # Turn 1: model calls ping_board. Turn 2: model replies with no more tool calls.
    call_count = {"n": 0}

    async def fake_create(*args, **kwargs):
        call_count["n"] += 1
        if call_count["n"] == 1:
            return _FakeResponse(_FakeMessage(
                content=None,
                tool_calls=[_FakeToolCall("call_1", "ping_board", "{}")],
            ))
        return _FakeResponse(_FakeMessage(content="Done.", tool_calls=[]))

    fake_openai_client = MagicMock()
    fake_openai_client.chat = MagicMock()
    fake_openai_client.chat.completions = MagicMock()
    fake_openai_client.chat.completions.create = AsyncMock(side_effect=fake_create)

    with patch("app.llm_clients.AsyncOpenAI", return_value=fake_openai_client), \
         patch("app.api.agent_routes._execute_tool", new_callable=AsyncMock) as mock_exec:
        mock_exec.side_effect = RuntimeError("boom — simulated tool crash")

        resp = client.post("/api/agent/chat", json={"message": "ping the board"})

    assert resp.status_code == 200
    body = resp.text

    events = []
    for chunk in body.split("\n\n"):
        chunk = chunk.strip()
        if chunk.startswith("data: "):
            events.append(json.loads(chunk[len("data: "):]))

    event_types = [e.get("event") for e in events]

    # The stream must have completed normally...
    assert "done" in event_types, f"Stream did not complete cleanly: {event_types}"
    # ...and must NOT have emitted a fatal 'error' event that ends the turn.
    assert "error" not in event_types, f"Stream crashed with an error event: {events}"

    # The failed tool call must show up as an honest, non-fatal failure.
    tool_results = [e for e in events if e.get("event") == "tool_result"]
    assert tool_results, f"Expected at least one tool_result event: {events}"
    failed = [e for e in tool_results if e.get("success") is False]
    assert failed, f"Expected a failed tool_result for the raised exception: {tool_results}"
    assert "ping_board" in failed[0]["summary"]
    assert "boom" in failed[0]["summary"]


# ══════════════════════════════════════════════════════════════════════════════
#  Chat block persistence — a page refresh must show the same capture/analysis
#  cards the user watched stream in live, not just the model's terse follow-up
#  text. Root cause (2026-08-24): only the FINAL model-generated reply text was
#  ever saved (ChatMessage.content); the rich tool_call/tool_result step blocks
#  — including image thumbnails — only ever existed in the browser's in-memory
#  React state and vanished on refresh. Fixed via ChatMessage.blocks_json,
#  populated by _mirror() in event_stream() as SSE events are yielded.
# ══════════════════════════════════════════════════════════════════════════════

async def test_assistant_reply_persists_structured_blocks_for_reload(db_session):
    """_persist_message + get_session_messages must round-trip a full block
    list (step + text) through blocks_json, not just the flat content string
    — this is the actual data-layer mechanism _mirror() in event_stream()
    relies on to make a page refresh show the same capture/analysis cards
    the user watched stream in live.

    (Exercised at the db_session/ORM layer rather than through TestClient:
    a pre-existing, unrelated aiosqlite+StaticPool+TestClient interaction
    makes chat_sessions/chat_messages invisible to TestClient's request
    thread specifically — reproduced in isolation with a bare
    POST /api/agent/sessions and no chat/blocks code involved at all, so
    it predates and is independent of this fix. db_session is the fixture
    every other DB-touching test in this file already uses reliably.)
    """
    from app.agent.models import ChatSession
    from app.api.agent_routes import _persist_message, get_session_messages

    session = ChatSession(board_id="stm32", name="test session")
    db_session.add(session)
    await db_session.commit()
    await db_session.refresh(session)

    blocks = [
        {"type": "step", "id": "call_1", "label": "Pinging board...",
         "status": "done", "summary": "Ping sent — board LEDs will flash"},
        {"type": "text", "text": "Board is alive."},
    ]
    await _persist_message(session.id, "assistant", "Board is alive.", blocks=blocks)

    messages = await get_session_messages(session.id)
    assistant = next(m for m in messages if m["role"] == "assistant")

    assert assistant["blocks"] is not None, (
        "blocks_json did not round-trip — reload would fall back to flat "
        "text only, reproducing the original bug"
    )
    block_types = [b["type"] for b in assistant["blocks"]]
    assert "step" in block_types
    assert "text" in block_types

    step = next(b for b in assistant["blocks"] if b["type"] == "step")
    assert step["status"] == "done"
    assert step["id"] == "call_1"
    assert step["summary"] == "Ping sent — board LEDs will flash"

    text_block = next(b for b in assistant["blocks"] if b["type"] == "text")
    assert text_block["text"] == "Board is alive."


async def test_legacy_message_without_blocks_json_falls_back_gracefully(db_session):
    """A ChatMessage row written before this fix (blocks_json IS NULL) must
    still round-trip through get_session_messages without erroring — the
    dashboard client is responsible for falling back to a single text block
    (reconstructBlocks() in AgentChat.tsx), but the server contract must not
    choke on NULL, and must return None (not "[]" or an error) so the client
    can tell "no blocks recorded" apart from "recorded as empty"."""
    from app.agent.models import ChatSession
    from app.api.agent_routes import _persist_message, get_session_messages

    session = ChatSession(board_id="stm32", name="test session")
    db_session.add(session)
    await db_session.commit()
    await db_session.refresh(session)

    await _persist_message(session.id, "assistant", "Legacy plain-text reply.")

    messages = await get_session_messages(session.id)
    assistant = next(m for m in messages if m["role"] == "assistant")
    assert assistant["content"] == "Legacy plain-text reply."
    assert assistant["blocks"] is None
