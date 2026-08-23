"""
Thesis IoT Server — Agentic Chat Endpoint (SSE)

Uses Claude Haiku with tool_use for reliable intent dispatch.
The LLM decides which tool to call based on the user's message,
then the server executes the tool and streams results.

Available tools:
  - create_schedule: NL → JSON schedule → MQTT to board
  - capture_now: Single immediate camera capture
  - capture_sequence: Multiple captures with ms-precision timing
  - ping_board: LED heartbeat sequence on the physical board
  - analyze_latest: Fetch and display the most recent AI analysis
  - get_board_status: Report board telemetry

SSE event types:
  - thinking: AI reasoning text
  - tool_call: Agent is executing a tool (spinner on frontend)
  - tool_result: Tool execution complete (spinner → checkmark)
  - reply: Final markdown response
  - error: Something went wrong
  - done: Stream complete
"""

import asyncio
import json
import re
import time
from datetime import datetime, timedelta

import anthropic
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, StreamingResponse
from sqlalchemy import select
from sqlalchemy.orm import selectinload

from app.agent.models import ChatMessage, ChatSession
from app.config import settings
from app.db.database import async_session
from app.mqtt.client import mqtt_client, send_board_command
from app.planning.engine import generate_plan
from app.benchmark import timing as _timing
from app.benchmark.ids import next_task_id

router = APIRouter(prefix="/agent", tags=["Agent"])


# ── Session CRUD ────────────────────────────────────────────

@router.get("/sessions")
async def list_sessions(board_id: str = "stm32-iot-cam-01"):
    """List all chat sessions for a board."""
    async with async_session() as db:
        result = await db.execute(
            select(ChatSession)
            .where(ChatSession.board_id == board_id)
            .order_by(ChatSession.created_at.desc())
        )
        sessions = list(result.scalars().all())

    return [
        {"id": s.id, "name": s.name, "boardId": s.board_id, "createdAt": s.created_at.isoformat()}
        for s in sessions
    ]


@router.post("/sessions")
async def create_session(request: Request):
    """Create a new chat session."""
    body = await request.json()
    board_id = body.get("boardId", "stm32-iot-cam-01")
    name = body.get("name", f"Session {datetime.now().strftime('%H:%M')}")

    async with async_session() as db:
        session = ChatSession(board_id=board_id, name=name)
        db.add(session)
        await db.commit()
        await db.refresh(session)

    return {"id": session.id, "name": session.name, "boardId": session.board_id, "createdAt": session.created_at.isoformat()}


@router.delete("/sessions/{session_id}")
async def delete_session(session_id: int):
    """Delete a chat session and all its messages."""
    async with async_session() as db:
        result = await db.execute(
            select(ChatSession).where(ChatSession.id == session_id)
        )
        session = result.scalar_one_or_none()
        if not session:
            return JSONResponse(status_code=404, content={"detail": "Session not found"})
        await db.delete(session)
        await db.commit()

    return {"ok": True}


@router.get("/sessions/{session_id}/messages")
async def get_session_messages(session_id: int):
    """Get all messages for a session."""
    async with async_session() as db:
        result = await db.execute(
            select(ChatSession)
            .options(selectinload(ChatSession.messages))
            .where(ChatSession.id == session_id)
        )
        session = result.scalar_one_or_none()
        if not session:
            return JSONResponse(status_code=404, content={"detail": "Session not found"})

    return [
        {"role": m.role, "content": m.content, "createdAt": m.created_at.isoformat()}
        for m in session.messages
    ]


@router.delete("/sessions/{session_id}/messages")
async def clear_session_messages(session_id: int):
    """Clear all messages in a session (like /clear)."""
    from sqlalchemy import delete as sql_delete

    async with async_session() as db:
        await db.execute(
            sql_delete(ChatMessage).where(ChatMessage.session_id == session_id)
        )
        await db.commit()

    return {"ok": True}


async def _persist_message(session_id: int, role: str, content: str):
    """Save a message to the database."""
    async with async_session() as db:
        db.add(ChatMessage(session_id=session_id, role=role, content=content))
        await db.commit()


async def _load_history(session_id: int, limit: int = 20) -> list[dict]:
    """Load recent messages from DB for Claude's context."""
    async with async_session() as db:
        result = await db.execute(
            select(ChatMessage)
            .where(ChatMessage.session_id == session_id)
            .order_by(ChatMessage.created_at.desc())
            .limit(limit)
        )
        messages = list(result.scalars().all())

    # Return in chronological order
    history = [
        {"role": m.role, "content": m.content}
        for m in reversed(messages)
    ]

    # Prepend truncation note if the full history was capped
    if len(messages) == limit:
        history.insert(0, {
            "role": "user",
            "content": "[Note: conversation history truncated to last 20 messages]",
        })

    return history


# ── World-state snapshot (ambient grounding, injected every chat turn) ──

async def _build_world_state_snapshot() -> str:
    """Assemble a compact, CURRENT snapshot of the system's state.

    This is injected fresh into every chat call (see agent_chat), independent
    of session_id, so the agent is always grounded in live reality even in a
    brand-new chat session — no cross-session chat-history merging needed.
    Kept intentionally compact (a few hundred tokens): it's grounding context,
    not a data dump. Deep dives still go through tools (list_schedules,
    analyze_latest, synthesize_schedule, get_board_status).
    """
    from sqlalchemy import func
    from app.analysis.models import AnalysisResult
    from app.scheduler.service import list_schedules as _list_schedules
    from app.mqtt.client import get_board_snapshot
    from app.db.database import async_session

    lines: list[str] = []

    # ── Board connectivity/power state ──
    try:
        board = get_board_snapshot()
        state = board.get("state", "unknown")
        lp_mode = board.get("lp_mode")
        sleep_mode = board.get("sleep_mode")
        power_bits = []
        if lp_mode:
            power_bits.append(f"lp_mode={lp_mode}")
        if sleep_mode:
            power_bits.append("sleeping")
        power_str = f" ({', '.join(power_bits)})" if power_bits else ""
        lines.append(f"Board: {state}{power_str}")
    except Exception as e:
        lines.append(f"Board: unknown (status lookup failed: {e})")

    async with async_session() as db:
        # ── Active schedule(s) ──
        try:
            schedules = await _list_schedules(db)
        except Exception:
            schedules = []
        active = [s for s in schedules if s.is_active]
        if active:
            for s in active:
                times = [t.time for t in s.tasks]
                time_range = f"{times[0]}–{times[-1]}" if len(times) > 1 else (times[0] if times else "—")
                objective = s.tasks[0].objective if s.tasks else ""
                lines.append(
                    f"Active schedule: #{s.id} \"{s.name}\" — {len(s.tasks)} task(s), {time_range}"
                    + (f", objective: {objective}" if objective else "")
                )
        else:
            lines.append("Active schedule: none")

        if schedules:
            lines.append(f"Total schedules in DB: {len(schedules)} (use list_schedules for the full list)")

        # ── Recent captures (last 24h) + last few findings ──
        try:
            cutoff = datetime.utcnow() - timedelta(hours=24)
            count_result = await db.execute(
                select(func.count(AnalysisResult.id)).where(AnalysisResult.created_at >= cutoff)
            )
            recent_count = count_result.scalar() or 0
        except Exception:
            recent_count = 0
        lines.append(f"Captures in last 24h: {recent_count}")

        try:
            recent_result = await db.execute(
                select(AnalysisResult).order_by(AnalysisResult.created_at.desc()).limit(5)
            )
            recent = list(recent_result.scalars().all())
        except Exception:
            recent = []

        if recent:
            flagged = [a for a in recent if a.flagged]
            if flagged:
                reasons = "; ".join(a.flag_reason or "notable" for a in flagged[:3])
                lines.append(f"{len(flagged)} of the last {len(recent)} captures were FLAGGED: {reasons}")
            lines.append("Recent findings (newest first):")
            for a in recent:
                flag_tag = " [FLAGGED]" if a.flagged else ""
                findings = (a.analysis or "")[:140]
                lines.append(f"  - task #{a.task_id}{flag_tag}: {findings}")
        else:
            lines.append("Recent findings: none yet")

    return "\n".join(lines)


# ── Tool Definitions (Claude tool_use format) ────────────

AGENT_TOOLS = [
    {
        "name": "create_schedule",
        "description": (
            "Create a LONG-DURATION monitoring schedule (2+ minutes). "
            "Uses HH:MM time format — CANNOT schedule at sub-minute precision. "
            "DO NOT use for durations under 2 minutes — use capture_sequence instead. "
            "The schedule is saved to the database and sent to the board via MQTT."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "prompt": {
                    "type": "string",
                    "description": "The monitoring request with your chosen duration and frequency. Include 'every X minutes for Y duration'.",
                },
            },
            "required": ["prompt"],
        },
    },
    {
        "name": "capture_now",
        "description": (
            "Take a single picture immediately. The full pipeline runs: "
            "capture → upload → AI analysis. Use for single snapshots."
        ),
        "input_schema": {
            "type": "object",
            "properties": {},
        },
    },
    {
        "name": "capture_sequence",
        "description": (
            "Take multiple pictures with millisecond-precision timing. "
            "Use this for ANY monitoring with sub-minute intervals OR durations under 2 minutes. "
            "Examples: 'every 20 seconds for 5 min' → count=16, interval_ms=20000. "
            "'monitor for 30 seconds' → count=4, interval_ms=7500. "
            "'monitor for 1 minute' → count=5, interval_ms=12000. "
            "'burst of 3 shots' → count=3, interval_ms=2000. "
            "YOU decide count and interval_ms based on the duration and interval."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "count": {
                    "type": "integer",
                    "description": "Number of pictures to take (2-16). Decide based on duration and interval.",
                },
                "interval_ms": {
                    "type": "integer",
                    "description": "Milliseconds between each capture (minimum 500). Decide based on requested interval.",
                },
                "objective": {
                    "type": "string",
                    "description": "What to monitor/look for in each capture.",
                },
            },
            "required": ["count", "interval_ms"],
        },
    },
    {
        "name": "ping_board",
        "description": (
            "Send a ping command to the board. The board flashes its LEDs "
            "in a distinctive pattern to confirm it's alive and responsive. "
            "Use when the user wants to verify the board is reachable."
        ),
        "input_schema": {
            "type": "object",
            "properties": {},
        },
    },
    {
        "name": "start_portal",
        "description": (
            "Force the board into WiFi setup (captive portal) mode. "
            "The board starts a local access point so the user can "
            "reconfigure WiFi credentials via a browser. Use when the "
            "user says 'setup mode', 'portal', 'reconfigure wifi', or "
            "'enter setup'."
        ),
        "input_schema": {
            "type": "object",
            "properties": {},
        },
    },
    {
        "name": "analyze_latest",
        "description": (
            "Retrieve the most recent AI visual analysis from the database. "
            "Shows the objective, findings, and recommendation from the last "
            "image that was analyzed by the multimodal LLM."
        ),
        "input_schema": {
            "type": "object",
            "properties": {},
        },
    },
    {
        "name": "get_board_status",
        "description": (
            "Get the current board status including online/offline state, "
            "firmware version, uptime, and capture count."
        ),
        "input_schema": {
            "type": "object",
            "properties": {},
        },
    },
    {
        "name": "activate_schedule",
        "description": (
            "Activate an existing (currently inactive) monitoring schedule. "
            "Use when the user says 'activate schedule', 'start schedule', 'resume schedule', "
            "'turn on schedule', or 'run schedule X'. "
            "This deactivates any other active schedule first. "
            "For brand-new schedules use create_schedule instead."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "schedule_id": {
                    "type": "integer",
                    "description": "ID of the schedule to activate.",
                },
            },
            "required": ["schedule_id"],
        },
    },
    {
        "name": "deactivate_schedule",
        "description": (
            "Deactivate (stop) a currently active monitoring schedule. "
            "Use when the user says 'stop monitoring', 'cancel schedule', "
            "'deactivate', 'turn off', or 'disable schedule'. "
            "If schedule_id is not provided, deactivates whichever schedule is currently active."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "schedule_id": {
                    "type": "integer",
                    "description": "ID of the schedule to deactivate. If omitted, deactivates the active schedule.",
                },
            },
        },
    },
    {
        "name": "synthesize_schedule",
        "description": (
            "Synthesize all AI vision analyses into an evolving conclusion. "
            "Reads all recent analyses, identifies patterns and changes over time, "
            "and produces a progressive insight report. Use when the user asks "
            "'what did you learn', 'summarize findings', or 'conclusions'."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "limit": {
                    "type": "integer",
                    "description": "Number of recent analyses to synthesize (default 10).",
                },
            },
        },
    },
    {
        "name": "modify_schedule",
        "description": (
            "Modify an EXISTING monitoring schedule — change its times, frequency, or objective. "
            "Use when user says 'change', 'update', 'reschedule', 'adjust', 'shift', or 'modify' "
            "a schedule that already exists. Re-generates the schedule from a new prompt. "
            "DO NOT use to create a brand-new schedule — use create_schedule for that."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "prompt": {
                    "type": "string",
                    "description": "New monitoring request to replace the schedule with.",
                },
                "schedule_id": {
                    "type": "integer",
                    "description": "ID of the schedule to modify. If omitted, modifies the currently active schedule.",
                },
            },
            "required": ["prompt"],
        },
    },
    {
        "name": "sleep_mode",
        "description": (
            "Toggle the board's sleep mode. Use when the user says 'sleep', 'standby', "
            "'low power', 'wake up', or 'wake the board'. "
            "Pass enabled=true to sleep, enabled=false to wake."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "enabled": {
                    "type": "boolean",
                    "description": "True to put the board to sleep, False to wake it up.",
                },
            },
            "required": ["enabled"],
        },
    },
    {
        "name": "delete_schedule",
        "description": (
            "Permanently DELETE a monitoring schedule from the database. "
            "Use when the user says 'delete schedule', 'remove schedule', or 'get rid of schedule X'. "
            "This is permanent — use deactivate_schedule to just stop it without deleting."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "schedule_id": {
                    "type": "integer",
                    "description": "ID of the schedule to delete.",
                },
            },
            "required": ["schedule_id"],
        },
    },
    {
        "name": "delete_image",
        "description": (
            "Delete a captured image and its AI analysis from storage and the database. "
            "Use when the user says 'delete image', 'remove photo', or refers to a specific image file."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "date": {
                    "type": "string",
                    "description": "Date folder of the image, e.g. '2026-04-25'.",
                },
                "filename": {
                    "type": "string",
                    "description": "Image filename, e.g. 'task_42_1745000000.jpg'.",
                },
            },
            "required": ["date", "filename"],
        },
    },
    {
        "name": "list_schedules",
        "description": (
            "List ALL monitoring schedules in the database — id, name, active status, "
            "task count, and time range — regardless of whether they're active. "
            "Use when the user asks 'what schedules do I have', 'list my schedules', "
            "'show all schedules', or needs to look further back than the schedules already "
            "summarized in your context. The active schedule and a summary are already visible "
            "to you at all times — only call this for a fuller inventory or to resolve an "
            "ambiguous reference to an inactive/older schedule."
        ),
        "input_schema": {
            "type": "object",
            "properties": {},
        },
    },
]


def _agent_tools_openai_format() -> list[dict]:
    """Convert AGENT_TOOLS (Anthropic input_schema format, the single source
    of truth) to OpenAI function-calling format for the OpenRouter chat
    tool-use loop. Computed at call time — no parallel tool list to drift."""
    return [
        {
            "type": "function",
            "function": {
                "name": t["name"],
                "description": t["description"],
                "parameters": t["input_schema"],
            },
        }
        for t in AGENT_TOOLS
    ]


AGENT_SYSTEM_PROMPT = """You are the IoT Visual Monitoring Agent — an action-first operator controlling a physical STM32 camera board over MQTT. Be decisive, concise, and always prefer DOING over explaining.

Personality: You are a proactive field operator. When the user asks anything that could involve the camera, TAKE THE SHOT. When in doubt between looking at old data vs capturing fresh data, always capture fresh. Never say "I can't" — find the closest tool that achieves the intent.

## Tool routing (follow EXACTLY)

capture_now (DEFAULT — use this most often):
- "what do you see" / "look" / "check" / "show me" / "see now" / ANY question about current state
- "take a picture" / "capture" / "snap" / "photo"
- ANY ambiguous request → capture_now (bias toward action)

capture_sequence (timed multi-shot, up to 16 captures, runs in BACKGROUND):
- "monitor for/the next X seconds/minute" → capture_sequence
- "burst" / "sequence" / "take N pictures" / "rapid"
- ANY request with sub-minute intervals (e.g. "every 10/20/30 seconds") → capture_sequence
- 30s → count=4, interval_ms=7500
- 1 min → count=5, interval_ms=12000
- 5 min every 20s → count=16, interval_ms=20000
- NEVER use capture_now for "monitor" requests.
- This runs in the background — images appear in the Gallery as they arrive.

create_schedule (duration 2+ min, minute-level intervals ONLY):
- "monitor for X minutes/hours" with intervals >= 1 minute
- Uses HH:MM — CANNOT do sub-minute intervals (every 10s, 20s, 30s)
- If user asks for sub-minute intervals, ALWAYS use capture_sequence instead.
- YOU decide frequency. Pass duration+frequency as prompt.

Other tools:
- activate_schedule: "activate schedule X" / "start schedule" / "resume schedule" / "run schedule X"
- deactivate_schedule: "stop" / "cancel" / "deactivate" / "turn off" / "disable" schedule
- delete_schedule: "delete schedule" / "remove schedule" / "get rid of schedule" (permanent removal)
- modify_schedule: "change the schedule" / "update monitoring" / "reschedule" / "adjust" / "shift" an EXISTING schedule
- sleep_mode(enabled=true): "sleep" / "standby" / "low power" / "hibernate"
- sleep_mode(enabled=false): "wake up" / "wake the board" / "turn on"
- delete_image: "delete image" / "remove photo" / "delete the picture" (needs date + filename)
- ping_board: "ping" / "alive" / "responsive"
- start_portal: "setup" / "portal" / "wifi config"
- get_board_status: "status" / "health" / "firmware" / "uptime"
- analyze_latest: ONLY when user explicitly says "last" / "previous" / "show the old analysis"
- synthesize_schedule: "summarize all" / "conclusions" / "what did you learn"
- list_schedules: "what schedules do I have" / "list schedules" / user refers to a schedule not
  covered by the world-state snapshot below (e.g. an older/inactive one)

## World state

Below your instructions you will also receive a WORLD STATE block, refreshed on every message,
showing the currently active schedule(s), recent capture counts, the last few analysis findings
(with any FLAGGED as notable), and live board connectivity/power state. ALWAYS use it FIRST to
resolve ambiguous references ("that schedule", "the active one", "the morning monitor", "stop it")
before asking the user to repeat an ID — the active schedule's id is right there. If a flagged
finding is relevant to what the user is asking, mention it. Only call list_schedules or
analyze_latest when the world state doesn't have enough detail (e.g. an inactive schedule from
days ago, or the user wants the FULL finding text).

## Rules

1. ACTION FIRST: Call the tool immediately. Don't ask clarifying questions unless truly ambiguous between two very different actions.
2. FRESH > STALE: If user asks "what do you see" or similar — ALWAYS capture_now. Never show old analysis for present-tense questions.
3. CONCISE: The streaming pipeline shows progress. Don't narrate what will happen. After results arrive, give a 1-2 sentence human summary.
4. MULTI-TOOL: You can call multiple tools in one response when appropriate.
5. NO EXCUSES: Every user intent maps to a tool. Execute it.
6. GROUND YOURSELF: Use the WORLD STATE block to resolve "which schedule"/"the active one" instead of guessing or asking — it's always current.
"""


def _sse_event(event: str, data: dict) -> str:
    return f"data: {json.dumps({'event': event, **data})}\n\n"


@router.post("/chat")
async def agent_chat(request: Request):
    body = await request.json()
    message = body.get("message", "").strip()
    session_id = body.get("sessionId")  # DB integer ID

    # Single production model now — no client-selectable model, see
    # dashboard AgentChat.tsx (model picker removed 2026-08-19).
    model_key = "openrouter"

    if not message:
        return StreamingResponse(
            iter([_sse_event("error", {"text": "Empty message"})]),
            media_type="text/event-stream",
        )

    # Validate session exists
    if session_id is not None:
        try:
            session_id = int(session_id)
        except (ValueError, TypeError):
            session_id = None

    async def event_stream():
        try:
            # Load conversation history from DB
            history = await _load_history(session_id) if session_id else []
            history.append({"role": "user", "content": message})

            # Persist user message
            if session_id:
                await _persist_message(session_id, "user", message)

            yield _sse_event("thinking", {"text": f"Processing: \"{message}\""})

            # ── Benchmark: generate run_id + pre-allocate task_id for capture_now ──
            # task_id is generated here (not in _capture_pipeline) so the planning
            # latency (t_plan_start/t_plan_end) can be correlated with the same row.
            _bench_run_id = _timing.new_run_id()
            _bench_task_id = next_task_id()

            # ── Call OpenRouter (OpenAI-compatible tool-calling) ──
            # Degraded keyword-matching path when no key is configured at all —
            # still routes captures through _capture_pipeline (see
            # _fallback_dispatch), so images always stream back correctly.
            if not settings.openrouter_api_key:
                async for ev in _fallback_dispatch(message, session_id):
                    yield ev
                yield _sse_event("done", {})
                return

            from openai import AsyncOpenAI

            client = AsyncOpenAI(
                base_url=settings.openrouter_base_url,
                api_key=settings.openrouter_api_key,
                timeout=30.0,
            )
            tools_schema = _agent_tools_openai_format()

            # ── World-state snapshot: fresh grounding for EVERY chat call, not
            # just once — this is what lets a brand-new session (or a mid-session
            # ambiguous reference like "that schedule") resolve against current
            # reality instead of guessing. See _build_world_state_snapshot().
            try:
                world_state = await _build_world_state_snapshot()
            except Exception as _e:
                world_state = f"(world state unavailable: {_e})"
            system_prompt = f"{AGENT_SYSTEM_PROMPT}\n\n## WORLD STATE (live, refreshed this turn)\n\n{world_state}"

            # ── Multi-turn agentic loop ───────────────────────────────────────
            MAX_AGENT_TURNS = 8
            messages_list = [{"role": "system", "content": system_prompt}] + list(history)
            loop_count = 0
            final_reply_parts: list[str] = []
            _t_plan_start = time.time()

            while loop_count < MAX_AGENT_TURNS:
                loop_count += 1

                # Stop mid-generation: client disconnect (Stop button on the
                # dashboard aborts its fetch) breaks the loop before the next
                # OpenRouter call or tool dispatch.
                if await request.is_disconnected():
                    return

                response = await client.chat.completions.create(
                    model=settings.openrouter_planner_model,
                    max_tokens=1024,
                    messages=messages_list,
                    tools=tools_schema,
                    temperature=0.1,
                )

                if loop_count == 1:
                    _t_plan_end = time.time()
                    # Persist planning timestamps for the first (planning) turn
                    await _timing.record(
                        _bench_task_id,
                        run_id=_bench_run_id,
                        t_plan_start=_t_plan_start,
                        t_plan_end=_t_plan_end,
                        model_key=model_key,
                    )

                choice = response.choices[0]
                reply_msg = choice.message
                tool_calls = reply_msg.tool_calls or []

                if reply_msg.content and reply_msg.content.strip():
                    final_reply_parts.append(reply_msg.content)
                    yield _sse_event("reply", {"text": reply_msg.content})

                if not tool_calls:
                    break  # No more tool calls — final turn

                messages_list.append({
                    "role": "assistant",
                    "content": reply_msg.content or "",
                    "tool_calls": [
                        {
                            "id": tc.id,
                            "type": "function",
                            "function": {
                                "name": tc.function.name,
                                "arguments": tc.function.arguments,
                            },
                        }
                        for tc in tool_calls
                    ],
                })

                for tc in tool_calls:
                    if await request.is_disconnected():
                        return

                    tool_name = tc.function.name
                    try:
                        tool_input = json.loads(tc.function.arguments) if tc.function.arguments else {}
                    except (json.JSONDecodeError, TypeError):
                        tool_input = {}
                    if not isinstance(tool_input, dict):
                        tool_input = {}

                    if tool_name == "capture_now":
                        pipeline_reply = ""
                        async for ev in _capture_pipeline(
                            tool_input, model_key,
                            bench_task_id=_bench_task_id, bench_run_id=_bench_run_id,
                        ):
                            yield ev
                            if '"event": "reply"' in ev:
                                try:
                                    ev_data = json.loads(ev.split("data: ", 1)[1].strip())
                                    pipeline_reply = ev_data.get("text", "")
                                except Exception as _e:
                                    print(f"[WARN] SSE reply-parse error: {_e}")
                        messages_list.append({
                            "role": "tool",
                            "tool_call_id": tc.id,
                            "content": pipeline_reply or "Capture pipeline completed.",
                        })

                    elif tool_name == "capture_sequence":
                        pipeline_reply = ""
                        async for ev in _capture_sequence_pipeline(
                            tool_input,
                            bench_run_id=_bench_run_id,
                            model_key=model_key,
                        ):
                            yield ev
                            if '"event": "reply"' in ev:
                                try:
                                    ev_data = json.loads(ev.split("data: ", 1)[1].strip())
                                    pipeline_reply = ev_data.get("text", "")
                                except Exception as _e:
                                    print(f"[WARN] SSE reply-parse error: {_e}")
                        messages_list.append({
                            "role": "tool",
                            "tool_call_id": tc.id,
                            "content": pipeline_reply or "Sequence pipeline completed.",
                        })

                    else:
                        yield _sse_event("tool_call", {
                            "id": tc.id,
                            "label": _tool_label(tool_name, tool_input),
                        })

                        result = await _execute_tool(tool_name, tool_input, session_id, model_key=model_key)

                        yield _sse_event("tool_result", {
                            "id": tc.id,
                            "success": result["success"],
                            "summary": result["summary"],
                        })

                        messages_list.append({
                            "role": "tool",
                            "tool_call_id": tc.id,
                            "content": json.dumps(result),
                        })

            # ── Persist final reply (only once, after loop completes) ─────────
            full_reply = "\n\n".join(p for p in final_reply_parts if p)
            if full_reply and session_id:
                await _persist_message(session_id, "assistant", full_reply)

            yield _sse_event("done", {})

        except Exception as e:
            yield _sse_event("error", {"text": str(e)})

    return StreamingResponse(
        event_stream(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


def _tool_label(name: str, inp: dict) -> str:
    labels = {
        "create_schedule": f"Generating schedule: \"{inp.get('prompt', '')[:60]}\"",
        "capture_now": "Sending capture command to board...",
        "capture_sequence": f"Sending {inp.get('count', '?')}-shot sequence...",
        "activate_schedule": f"Activating schedule #{inp.get('schedule_id', '?')}...",
        "deactivate_schedule": "Deactivating schedule...",
        "delete_schedule": f"Deleting schedule #{inp.get('schedule_id', '?')}...",
        "modify_schedule": f"Updating schedule: \"{inp.get('prompt', '')[:50]}\"",
        "sleep_mode": "Sleeping board..." if inp.get("enabled") else "Waking board...",
        "delete_image": f"Deleting {inp.get('filename', 'image')}...",
        "ping_board": "Pinging board...",
        "start_portal": "Starting WiFi portal...",
        "analyze_latest": "Fetching latest AI analysis...",
        "get_board_status": "Checking board status...",
        "synthesize_schedule": "Synthesizing schedule...",
        "list_schedules": "Listing schedules...",
    }
    return labels.get(name, f"Executing {name}...")


async def _capture_pipeline(
    tool_input: dict,
    model_key: str = "claude-sonnet",
    bench_task_id: int | None = None,
    bench_run_id: str | None = None,
):
    """Full agentic pipeline: capture → wait for upload(s) → analysis → report.

    Two-phase design:
      Phase 1 (20s): watch uploads directory for new .jpg files — detects upload
                     independently of task_id format (firmware may truncate to uint16).
      Phase 2: run analysis INLINE with the user's chosen model_key. This ensures
               the selected chat model (Haiku / Sonnet) also drives image analysis,
               enabling thesis benchmarking. The background _run_analysis task in
               routes.py skips files already analyzed by the pipeline.

    bench_task_id / bench_run_id: passed from event_stream so planning timestamps
    (recorded there) and pipeline timestamps land in the same CaptureLatency row.
    """
    import re as _re
    from pathlib import Path
    from sqlalchemy import select
    from app.analysis.models import AnalysisResult
    from app.analysis.engine import analyze_image as _analyze_image
    from app.db.database import async_session

    # Use the pre-allocated task_id from event_stream when available so all
    # benchmark timestamps (planning + pipeline) share the same row.
    task_id = bench_task_id if bench_task_id is not None else next_task_id()
    run_id = bench_run_id if bench_run_id is not None else _timing.new_run_id()

    # ── Benchmark: record pipeline entry ─────────────────────────────────────
    _t_request = time.time()
    await _timing.record(
        task_id,
        run_id=run_id,
        capture_type="single",
        model_key=model_key,
        t_request=_t_request,
    )

    expected = 1
    total_seq_ms = 0
    command = json.dumps({"type": "capture_now", "task_id": task_id})
    yield _sse_event("tool_call", {"id": "capture", "label": "Sending capture command to board..."})

    send_board_command(mqtt_client, settings.mqtt_topic_commands, command)
    # ── Benchmark: MQTT sent ──────────────────────────────────────────────────
    await _timing.record(task_id, t_mqtt_sent=time.time())

    yield _sse_event("tool_result", {"id": "capture", "success": True, "summary": f"Capture command sent (task #{task_id})"})

    # ── Phase 1: wait for image file(s) to land on disk (20s) ────────────────
    start_time = datetime.utcnow()   # SQLite stores UTC via func.now(); local time would miss rows
    # Snapshot existing files before the loop using current local date
    _init_today = datetime.now().strftime("%Y-%m-%d")
    _init_upload_dir = Path(f"./data/uploads/{_init_today}")
    existing_files = set(_init_upload_dir.glob("*.jpg")) if _init_upload_dir.exists() else set()

    wait_label = f"Waiting for {'image' if expected == 1 else f'{expected} images'}..."
    yield _sse_event("tool_call", {"id": "upload", "label": wait_label})

    image_received = False
    current_files = existing_files.copy()
    for poll_idx in range(20):
        await asyncio.sleep(1)
        elapsed = poll_idx + 1

        if elapsed % 5 == 0:
            yield _sse_event("tool_update", {"id": "upload", "label": f"{wait_label} ({elapsed}s)"})

        # Recompute date/dir on each poll so midnight rollovers are handled
        today = datetime.now().strftime("%Y-%m-%d")
        upload_dir = Path(f"./data/uploads/{today}")

        if upload_dir.exists():
            current_files = set(upload_dir.glob("*.jpg"))
            if len(current_files - existing_files) >= expected:
                image_received = True
                break

    if not image_received:
        await _timing.record(task_id, success=False, error="Timeout after 20s waiting for upload")
        yield _sse_event("tool_result", {
            "id": "upload",
            "success": False,
            "summary": "Timeout after 20s — board may be offline or firmware not current",
        })
        yield _sse_event("reply", {"text": "**Capture timed out.** The board didn't upload an image within the expected window. Check that it's online and the firmware is current."})
        return

    new_files = sorted(current_files - existing_files)[:expected]
    yield _sse_event("tool_result", {"id": "upload", "success": True, "summary": f"{len(new_files)} image{'s' if len(new_files) > 1 else ''} received"})

    # ── Phase 2: analyze inline with the user's chosen model ─────────────────
    # Running analysis here (not in the background upload task) ensures the selected
    # model (Haiku / Sonnet / etc.) is used. The background _run_analysis in routes.py
    # checks for an existing row and skips this file to avoid double-analysis.
    analyses = []
    yield _sse_event("tool_call", {"id": "analyze", "label": f"Analyzing with {model_key}..."})

    # ── Benchmark: analysis start ─────────────────────────────────────────────
    await _timing.record(task_id, t_analysis_start=time.time())

    _analysis_error: str | None = None
    for idx, new_file in enumerate(new_files, 1):
        file_path = str(new_file)
        _m = _re.search(r"task_(\d+)_", new_file.name)
        board_task_id = int(_m.group(1)) if _m else 0

        try:
            analysis_result = await _analyze_image(file_path, "General visual inspection", model_key)
        except Exception as exc:
            _analysis_error = f"{type(exc).__name__}: {exc}"
            analysis_result = {
                "findings": f"Analysis error: {_analysis_error}",
                "recommendation": "Check AI backend configuration (OPENROUTER_API_KEY, ANTHROPIC_API_KEY, GEMINI_API_KEY, or vLLM).",
                "description": "",
                "model_used": model_key,
                "inference_time_ms": 0,
            }

        async with async_session() as db:
            db_row = AnalysisResult(
                task_id=board_task_id,
                image_path=file_path,
                objective="General visual inspection",
                analysis=analysis_result.get("findings", ""),
                recommendation=analysis_result.get("recommendation", ""),
                model_used=analysis_result.get("model_used", model_key),
                inference_time_ms=analysis_result.get("inference_time_ms", 0),
                flagged=analysis_result.get("flagged", False),
                flag_reason=analysis_result.get("flag_reason", ""),
            )
            db.add(db_row)
            await db.commit()
            await db.refresh(db_row)

        analyses.append(db_row)
        yield _sse_event("tool_call", {
            "id": f"img_{idx}",
            "label": f"Image {idx}/{expected} received...",
        })
        yield _sse_event("tool_result", {
            "id": f"img_{idx}",
            "success": True,
            "summary": f"Image {idx}/{expected} analyzed (task #{board_task_id})",
            "image_url": f"/api/images/{new_file.parent.name}/{new_file.name}",
        })

    # ── Benchmark: analysis end ───────────────────────────────────────────────
    await _timing.record(
        task_id,
        t_analysis_end=time.time(),
        success=(_analysis_error is None),
        error=_analysis_error,
    )

    yield _sse_event("tool_result", {
        "id": "analyze",
        "success": True,
        "summary": f"{len(analyses)}/{expected} image{'s' if expected > 1 else ''} analyzed",
    })

    # Build report — embed image URL(s) in reply text so they persist after page refresh
    if len(analyses) == 1:
        a = analyses[0]
        _img_url = f"/api/images/{new_files[0].parent.name}/{new_files[0].name}" if new_files else ""
        _img_md = f"![Captured]({_img_url})\n\n" if _img_url else ""
        detail = (
            f"**Capture & Analysis Complete** (task #{a.task_id})\n\n"
            f"{_img_md}"
            f"**Objective:** {a.objective}\n\n"
            f"**Findings:** {a.analysis}\n\n"
            f"**Recommendation:** {a.recommendation}\n\n"
            f"*{a.model_used} | {a.inference_time_ms:.0f}ms*"
        )
    else:
        parts = [f"**Sequence Complete** — {len(analyses)}/{expected} images analyzed\n"]
        for i, (a, nf) in enumerate(zip(analyses, new_files), 1):
            _img_url = f"/api/images/{nf.parent.name}/{nf.name}"
            parts.append(
                f"**#{i}** (task {a.task_id}): {a.analysis[:120]}{'...' if len(a.analysis) > 120 else ''}\n"
                f"![Image {i}]({_img_url})"
            )
        if analyses[-1].recommendation:
            parts.append(f"\n**Recommendation:** {analyses[-1].recommendation}")
        parts.append(f"\n*{analyses[-1].model_used} | avg {sum(a.inference_time_ms for a in analyses) / len(analyses):.0f}ms*")
        detail = "\n\n".join(parts)

    # ── Benchmark: SSE delivered ──────────────────────────────────────────────
    await _timing.record(task_id, t_sse_delivered=time.time())

    yield _sse_event("reply", {"text": detail})


async def _capture_sequence_pipeline(tool_input: dict, bench_run_id: str | None = None, model_key: str = "claude-sonnet"):
    """
    Proper agentic sequence pipeline:
      1. Create DB schedule (real task IDs for objective lookup)
      2. Send MQTT capture_sequence with first task's DB ID
      3. Poll for all N analysis results as they arrive
      4. Stream per-image SSE events in real time

    This replaces the old fire-and-forget _tool_capture_sequence path.
    bench_run_id: correlates with the planning row recorded in event_stream.
    """
    from pathlib import Path
    from app.analysis.models import AnalysisResult
    from app.scheduler.service import create_schedule, activate_schedule
    from app.scheduler.notify import notify_schedule_update

    count = max(2, min(tool_input.get("count", 3), 16))
    interval = max(500, tool_input.get("interval_ms", 2000))
    objective = tool_input.get("objective", "Quick monitoring sequence")
    total_s = (count - 1) * interval / 1000

    # Create DB schedule so every upload can look up its objective by task_id
    now = datetime.now()
    tasks = []
    for i in range(count):
        offset_s = i * interval / 1000
        task_time = now + timedelta(seconds=offset_s)
        tasks.append({
            "time": task_time.strftime("%H:%M:%S"),
            "action": "CAPTURE_IMAGE",
            "objective": objective,
        })

    async with async_session() as db:
        schedule = await create_schedule(
            db,
            name=f"Quick: {count} shots over {total_s:.0f}s",
            description=f"Automated {count}-capture sequence at {interval}ms intervals",
            tasks=tasks,
        )
        await activate_schedule(db, schedule.id)

    await notify_schedule_update()

    first_task_id = schedule.tasks[0].id
    _bench_run_id = bench_run_id if bench_run_id is not None else _timing.new_run_id()

    # ── Benchmark: record sequence pipeline entry ─────────────────────────────
    await _timing.record(
        first_task_id,
        run_id=_bench_run_id,
        capture_type="sequence",
        model_key=model_key,
        t_request=time.time(),
    )

    delays = [i * interval for i in range(count)]
    command = json.dumps({
        "type": "capture_sequence",
        "task_id": first_task_id,
        "delays_ms": delays,
    })

    yield _sse_event("tool_call", {
        "id": "capture",
        "label": f"Sending {count}-shot sequence ({interval}ms apart)...",
    })
    send_board_command(mqtt_client, settings.mqtt_topic_commands, command)
    # ── Benchmark: MQTT sent ──────────────────────────────────────────────────
    await _timing.record(first_task_id, t_mqtt_sent=time.time())

    yield _sse_event("tool_result", {
        "id": "capture",
        "success": True,
        "summary": f"Sequence started (task #{first_task_id})",
    })

    # Poll for N analyses — all uploads arrive with task_id = first_task_id
    start_time = datetime.now()
    total_seq_ms = delays[-1] + 5000  # last capture + upload + analysis headroom
    timeout_polls = max(20, (total_seq_ms // 1000) + (count * 10))
    analyses = []
    seen_ids: set[int] = set()
    seq_label = f"Waiting for {count} image{'s' if count > 1 else ''}..."

    yield _sse_event("tool_call", {"id": "upload", "label": seq_label})

    _first_analysis_seen = False
    for poll_idx in range(int(timeout_polls)):
        await asyncio.sleep(1)
        elapsed = poll_idx + 1

        if elapsed % 5 == 0 and elapsed < timeout_polls:
            yield _sse_event("tool_update", {
                "id": "upload",
                "label": f"{seq_label} ({elapsed}s, {len(analyses)}/{count})",
            })

        async with async_session() as db:
            result = await db.execute(
                select(AnalysisResult)
                .where(AnalysisResult.created_at >= start_time)
                .where(AnalysisResult.task_id == first_task_id)
                .order_by(AnalysisResult.created_at.asc())
            )
            new_results = [r for r in result.scalars().all() if r.id not in seen_ids]

        for a in new_results:
            seen_ids.add(a.id)
            analyses.append(a)

            # ── Benchmark: record first analysis completion ───────────────────
            if not _first_analysis_seen:
                _first_analysis_seen = True
                await _timing.record(first_task_id, t_analysis_end=time.time())

            _img_path = Path(a.image_path)
            _img_date = _img_path.parent.name
            _img_file = _img_path.name
            _img_step_id = f"img_{len(analyses)}"
            yield _sse_event("tool_call", {
                "id": _img_step_id,
                "label": f"Image {len(analyses)}/{count} received...",
            })
            yield _sse_event("tool_result", {
                "id": _img_step_id,
                "success": True,
                "summary": f"Image {len(analyses)}/{count} analyzed (task #{a.task_id})",
                "image_url": f"/api/images/{_img_date}/{_img_file}",
            })

        if len(analyses) >= count:
            break

    if not analyses:
        await _timing.record(first_task_id, success=False, error="Timeout — no analyses received")
        yield _sse_event("tool_result", {
            "id": "upload",
            "success": False,
            "summary": "Timeout — board may be offline",
        })
        yield _sse_event("reply", {
            "text": "**Sequence sent** but no images received yet. The board may be offline or busy.",
        })
        return

    yield _sse_event("tool_result", {
        "id": "upload",
        "success": True,
        "summary": f"{len(analyses)}/{count} image{'s' if count > 1 else ''} analyzed",
    })

    # Build per-image report
    parts = [f"**Sequence Complete** — {len(analyses)}/{count} images analyzed\n"]
    for i, a in enumerate(analyses, 1):
        snippet = a.analysis[:120] + ("..." if len(a.analysis) > 120 else "")
        parts.append(f"**#{i}**: {snippet}")
    if analyses[-1].recommendation:
        parts.append(f"\n**Recommendation:** {analyses[-1].recommendation}")
    avg_ms = sum(a.inference_time_ms for a in analyses) / len(analyses)
    parts.append(f"\n*{analyses[-1].model_used} | avg {avg_ms:.0f}ms*")

    # ── Benchmark: SSE delivered ──────────────────────────────────────────────
    await _timing.record(first_task_id, t_sse_delivered=time.time(), success=True)

    yield _sse_event("reply", {"text": "\n\n".join(parts)})


async def _execute_tool(name: str, inp: dict, session_id: str, model_key: str = "claude-haiku") -> dict:
    """Execute a tool and return {success, summary, detail}."""

    if name == "create_schedule":
        return await _tool_create_schedule(inp)
    elif name == "capture_now":
        return await _tool_capture_now()
    elif name == "capture_sequence":
        return await _tool_capture_sequence(inp)
    elif name == "activate_schedule":
        return await _tool_activate_schedule(inp)
    elif name == "deactivate_schedule":
        return await _tool_deactivate_schedule(inp)
    elif name == "modify_schedule":
        return await _tool_modify_schedule(inp)
    elif name == "ping_board":
        return await _tool_ping()
    elif name == "start_portal":
        return await _tool_start_portal()
    elif name == "analyze_latest":
        return await _tool_analyze_latest()
    elif name == "get_board_status":
        return await _tool_board_status()
    elif name == "synthesize_schedule":
        return await _tool_synthesize(inp, model_key=model_key)
    elif name == "sleep_mode":
        return await _tool_sleep_mode(inp)
    elif name == "delete_schedule":
        return await _tool_delete_schedule(inp)
    elif name == "delete_image":
        return await _tool_delete_image(inp)
    elif name == "list_schedules":
        return await _tool_list_schedules()
    else:
        return {"success": False, "summary": f"Unknown tool: {name}", "detail": ""}


async def _tool_create_schedule(inp: dict) -> dict:
    from app.db.database import async_session
    from app.scheduler.service import create_schedule, activate_schedule, list_schedules

    prompt = inp.get("prompt", "")
    model_key = (
        "openrouter" if settings.openrouter_api_key
        else "claude-sonnet" if settings.anthropic_api_key
        else "qwen3-vl"
    )

    # Enrich prompt with current time so the planner avoids past times
    now = datetime.now()
    enriched_prompt = (
        f"Current time: {now.strftime('%H:%M')} on {now.strftime('%Y-%m-%d')}. "
        f"Request: {prompt}"
    )

    try:
        plan = await generate_plan(enriched_prompt, model_key)
    except Exception as e:
        return {"success": False, "summary": f"Planning failed: {e}", "detail": ""}

    if not plan.tasks:
        return {"success": False, "summary": "No tasks generated", "detail": "The planner returned an empty schedule."}

    task_count = len(plan.tasks)
    times = [t.time for t in plan.tasks]

    # Persist to DB so analysis can find objectives by task_id
    async with async_session() as db:
        # Check for conflicting active schedules
        existing = await list_schedules(db)
        active = [s for s in existing if s.is_active]
        conflict_note = ""
        if active:
            conflict_note = f"\n\n*Deactivated previous schedule: \"{active[0].name}\"*"

        schedule = await create_schedule(
            db,
            name=f"Agent: {prompt[:50]}",
            description=prompt,
            tasks=[t.model_dump() for t in plan.tasks],
        )

        # Activate (deactivates others) and get MQTT payload
        mqtt_payload = await activate_schedule(db, schedule.id)

    # Publish to board
    send_board_command(mqtt_client, settings.mqtt_topic_commands, json.dumps(mqtt_payload))

    # Push real-time update to dashboard
    from app.scheduler.notify import notify_schedule_update
    await notify_schedule_update()

    task_list = "\n".join(
        f"| {t.id} | {t.time} | {t.action} | {t.objective} |"
        for t in plan.tasks
    )
    detail = (
        f"**Schedule active** ({task_count} tasks, {times[0]}–{times[-1]})\n\n"
        f"| ID | Time | Action | Objective |\n|---|---|---|---|\n{task_list}"
        f"{conflict_note}"
    )

    return {
        "success": True,
        "summary": f"{task_count} tasks scheduled ({times[0]}–{times[-1]})",
        "detail": detail,
    }


async def _tool_activate_schedule(inp: dict) -> dict:
    from app.db.database import async_session
    from app.scheduler.service import activate_schedule, get_schedule
    from app.scheduler.notify import notify_schedule_update

    schedule_id = inp.get("schedule_id")
    if not schedule_id:
        return {"success": False, "summary": "schedule_id required", "detail": ""}

    async with async_session() as db:
        try:
            mqtt_payload = await activate_schedule(db, int(schedule_id))
            schedule = await get_schedule(db, int(schedule_id))
            name = schedule.name
        except Exception as e:
            return {"success": False, "summary": f"Activate failed: {e}", "detail": ""}

    send_board_command(mqtt_client, settings.mqtt_topic_commands, json.dumps(mqtt_payload))
    await notify_schedule_update()

    return {
        "success": True,
        "summary": f"Activated: {name}",
        "detail": f"**Schedule activated:** \"{name}\"\n\nThe board has been sent the updated task list.",
    }


async def _tool_deactivate_schedule(inp: dict) -> dict:
    from app.db.database import async_session
    from app.scheduler.service import deactivate_schedule, list_schedules
    from app.scheduler.notify import notify_schedule_update

    schedule_id = inp.get("schedule_id")

    async with async_session() as db:
        if schedule_id:
            schedule = await deactivate_schedule(db, schedule_id)
            name = schedule.name
        else:
            # Find and deactivate the currently active schedule
            schedules = await list_schedules(db)
            active = [s for s in schedules if s.is_active]
            if not active:
                return {"success": False, "summary": "No active schedule", "detail": "There is no active schedule to deactivate."}
            schedule = await deactivate_schedule(db, active[0].id)
            name = schedule.name

    # Tell board to clear its schedule
    command = json.dumps({"type": "delete_schedule"})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, command)

    await notify_schedule_update()

    return {
        "success": True,
        "summary": f"Deactivated: {name}",
        "detail": f"**Schedule deactivated:** \"{name}\"\n\nThe board has been told to clear its schedule.",
    }


async def _tool_modify_schedule(inp: dict) -> dict:
    """Re-plan and update an existing schedule in-place."""
    from app.db.database import async_session
    from app.scheduler.service import get_schedule, list_schedules, update_schedule, activate_schedule
    from app.scheduler.notify import notify_schedule_update

    prompt = inp.get("prompt", "")
    schedule_id = inp.get("schedule_id")

    # ── Step 1: read target schedule ──────────────────────────────────────────
    async with async_session() as db:
        if schedule_id:
            try:
                schedule = await get_schedule(db, int(schedule_id))
            except Exception:
                return {"success": False, "summary": f"Schedule #{schedule_id} not found", "detail": ""}
        else:
            schedules = await list_schedules(db)
            active = [s for s in schedules if s.is_active]
            if not active:
                if schedules:
                    schedule = schedules[0]
                else:
                    return {
                        "success": False,
                        "summary": "No schedule found to modify",
                        "detail": "There are no schedules. Create one first with create_schedule.",
                    }
            else:
                schedule = active[0]
            schedule_id = schedule.id

        was_active = schedule.is_active
        old_name = schedule.name

    # ── Step 2: LLM re-planning (outside session — async-safe) ───────────────
    now = datetime.now()
    enriched_prompt = (
        f"Current time: {now.strftime('%H:%M')} on {now.strftime('%Y-%m-%d')}. "
        f"Request: {prompt}"
    )
    model_key = (
        "openrouter" if settings.openrouter_api_key
        else "claude-sonnet" if settings.anthropic_api_key
        else "qwen3-vl"
    )
    try:
        plan = await generate_plan(enriched_prompt, model_key)
    except Exception as e:
        return {"success": False, "summary": f"Re-planning failed: {e}", "detail": ""}

    if not plan.tasks:
        return {"success": False, "summary": "Planner returned empty schedule", "detail": ""}

    # ── Step 3: write update in a single new session ──────────────────────────
    mqtt_payload = None
    async with async_session() as db:
        try:
            await update_schedule(
                db,
                schedule_id=int(schedule_id),
                name=f"Agent: {prompt[:50]}",
                description=prompt,
                tasks=[t.model_dump() for t in plan.tasks],
            )
        except Exception:
            # Schedule was deleted between read and write
            return {"success": False, "summary": f"Schedule #{schedule_id} was deleted concurrently", "detail": ""}
        if was_active:
            mqtt_payload = await activate_schedule(db, int(schedule_id))

    if was_active and mqtt_payload:
        send_board_command(mqtt_client, settings.mqtt_topic_commands, json.dumps(mqtt_payload))

    await notify_schedule_update()

    times = [t.time for t in plan.tasks]
    task_list = "\n".join(
        f"| {t.id} | {t.time} | {t.action} | {t.objective} |"
        for t in plan.tasks
    )
    detail = (
        f"**Schedule updated** (was: \"{old_name}\")\n\n"
        f"**New plan:** {len(plan.tasks)} tasks, {times[0]}–{times[-1]}\n\n"
        f"| ID | Time | Action | Objective |\n|---|---|---|---|\n{task_list}"
        + (f"\n\n*Board notified with updated schedule.*" if was_active else "")
    )
    return {
        "success": True,
        "summary": f"Schedule #{schedule_id} updated: {len(plan.tasks)} tasks ({times[0]}–{times[-1]})",
        "detail": detail,
    }


async def _tool_capture_now() -> dict:
    task_id = next_task_id()
    command = json.dumps({"type": "capture_now", "task_id": task_id})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, command)
    return {
        "success": True,
        "summary": f"Capture sent (task #{task_id})",
        "detail": f"**Capture triggered** (task #{task_id}). The image will appear in the gallery after upload + AI analysis.",
    }


async def _tool_capture_sequence(inp: dict) -> dict:
    from app.db.database import async_session
    from app.scheduler.service import create_schedule, activate_schedule

    count = max(2, min(inp.get("count", 3), 16))
    interval = max(500, inp.get("interval_ms", 2000))
    total_s = (count - 1) * interval / 1000

    # Build schedule tasks — use current time offset for display
    now = datetime.now()
    tasks = []
    for i in range(count):
        offset_s = i * interval / 1000
        task_time = now + timedelta(seconds=offset_s)
        tasks.append({
            "time": task_time.strftime("%H:%M:%S"),
            "action": "CAPTURE_IMAGE",
            "objective": inp.get("objective", "Quick monitoring sequence"),
        })

    # Persist as a real schedule
    async with async_session() as db:
        schedule = await create_schedule(
            db,
            name=f"Quick: {count} shots over {total_s:.0f}s",
            description=f"Automated {count}-capture sequence at {interval}ms intervals",
            tasks=tasks,
        )
        await activate_schedule(db, schedule.id)

    # Push real-time update to dashboard
    from app.scheduler.notify import notify_schedule_update
    await notify_schedule_update()

    # Send the capture_sequence MQTT command (ms-precision timing)
    delays = [i * interval for i in range(count)]
    command = json.dumps({
        "type": "capture_sequence",
        "task_id": schedule.tasks[0].id,
        "delays_ms": delays,
    })
    send_board_command(mqtt_client, settings.mqtt_topic_commands, command)

    return {
        "success": True,
        "summary": f"{count} captures scheduled ({total_s:.0f}s sequence)",
        "detail": (
            f"**Monitoring started** — {count} captures over {total_s:.0f}s, running in the background.\n\n"
            f"Track progress in the Schedules tab."
        ),
    }


async def _tool_ping() -> dict:
    command = json.dumps({"type": "ping"})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, command)
    return {
        "success": True,
        "summary": "Ping sent — board LEDs will flash",
        "detail": "**Ping sent.** The board's LEDs will flash to confirm it's alive.",
    }


async def _tool_start_portal() -> dict:
    command = json.dumps({"type": "start_portal"})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, command)
    return {
        "success": True,
        "summary": "Portal mode started — board is now an access point",
        "detail": (
            "**Setup mode activated.** The board is starting a WiFi access point.\n\n"
            "Connect to the board's AP network and open `http://192.168.10.1` "
            "in a browser to reconfigure WiFi credentials."
        ),
    }


async def _tool_analyze_latest() -> dict:
    from sqlalchemy import select
    from app.analysis.models import AnalysisResult
    from app.db.database import async_session

    async with async_session() as db:
        result = await db.execute(
            select(AnalysisResult).order_by(AnalysisResult.created_at.desc()).limit(1)
        )
        analysis = result.scalar_one_or_none()

    if not analysis:
        return {
            "success": False,
            "summary": "No analyses found",
            "detail": "No image analyses found yet. Try capturing an image first.",
        }

    return {
        "success": True,
        "summary": f"Analysis for task #{analysis.task_id}",
        "detail": (
            f"**Latest Analysis** (task #{analysis.task_id})\n\n"
            f"**Objective:** {analysis.objective}\n\n"
            f"**Findings:** {analysis.analysis}\n\n"
            f"**Recommendation:** {analysis.recommendation}\n\n"
            f"*{analysis.model_used} | {analysis.inference_time_ms:.0f}ms*"
        ),
    }


async def _tool_board_status() -> dict:
    from sqlalchemy import func, select
    from app.analysis.models import AnalysisResult
    from app.db.database import async_session

    # Count analyses and images to give real stats
    async with async_session() as db:
        count_result = await db.execute(select(func.count(AnalysisResult.id)))
        analysis_count = count_result.scalar() or 0

        latest_result = await db.execute(
            select(AnalysisResult).order_by(AnalysisResult.created_at.desc()).limit(1)
        )
        latest = latest_result.scalar_one_or_none()

    detail = "**Board Status**\n\n"
    detail += f"- **Total Analyses:** {analysis_count}\n"
    if latest:
        detail += f"- **Last Analysis:** task #{latest.task_id} ({latest.model_used}, {latest.inference_time_ms:.0f}ms)\n"
        detail += f"- **Last Objective:** {latest.objective}\n"

    return {
        "success": True,
        "summary": f"{analysis_count} analyses recorded",
        "detail": detail,
    }


async def _tool_synthesize(inp: dict, model_key: str = "claude-haiku") -> dict:
    """Synthesize all recent analyses into evolving conclusions."""
    from sqlalchemy import select
    from app.analysis.models import AnalysisResult
    from app.db.database import async_session

    limit = inp.get("limit", 10)

    async with async_session() as db:
        result = await db.execute(
            select(AnalysisResult)
            .order_by(AnalysisResult.created_at.desc())
            .limit(limit)
        )
        analyses = list(result.scalars().all())

    if not analyses:
        return {
            "success": False,
            "summary": "No analyses to synthesize",
            "detail": "No image analyses found. Capture some images first.",
        }

    # Build a synthesis — OpenRouter (production default) preferred, Claude
    # kept for benchmark/manual model_key selection, plain aggregation last.
    if settings.openrouter_api_key or settings.anthropic_api_key:
        entries = []
        flagged_count = 0
        for a in reversed(analyses):  # chronological order
            flag_tag = ""
            if a.flagged:
                flagged_count += 1
                flag_tag = f" [FLAGGED: {a.flag_reason}]" if a.flag_reason else " [FLAGGED]"
            entries.append(
                f"Task #{a.task_id}{flag_tag} | {a.objective}\n"
                f"Findings: {a.analysis}\n"
                f"Recommendation: {a.recommendation}"
            )

        flagged_note = (
            f"\n\n{flagged_count} of {len(entries)} observations were FLAGGED as notable — "
            "weight these more heavily in your synthesis and call them out explicitly."
            if flagged_count else ""
        )
        synthesis_prompt = (
            "You are analyzing a series of visual monitoring observations from an IoT camera over time.\n\n"
            "Observations (chronological, [FLAGGED] marks ones the analysis engine judged notable):\n\n" +
            "\n---\n".join(entries) +
            flagged_note +
            "\n\nSynthesize these observations into:\n"
            "1. **Pattern**: What patterns or trends do you see across observations?\n"
            "2. **Changes**: What changed between observations?\n"
            "3. **Conclusion**: Overall assessment of the monitored environment.\n"
            "4. **Recommendation**: What should be done next?\n\n"
            "Be concise and specific."
        )

        if settings.openrouter_api_key:
            from openai import AsyncOpenAI

            client = AsyncOpenAI(
                base_url=settings.openrouter_base_url,
                api_key=settings.openrouter_api_key,
            )
            response = await client.chat.completions.create(
                model=settings.openrouter_planner_model,
                max_tokens=1024,
                messages=[{"role": "user", "content": synthesis_prompt}],
                temperature=0.2,
            )
            synthesis = response.choices[0].message.content
        else:
            CLAUDE_MODEL_MAP = {
                "claude-haiku": settings.claude_haiku_model,
                "claude-sonnet": settings.claude_sonnet_model,
            }
            resolved_synthesis_model = CLAUDE_MODEL_MAP.get(model_key) or settings.claude_haiku_model
            client = anthropic.AsyncAnthropic(api_key=settings.anthropic_api_key)
            response = await client.messages.create(
                model=resolved_synthesis_model,
                max_tokens=1024,
                messages=[{"role": "user", "content": synthesis_prompt}],
                temperature=0.2,
            )
            synthesis = response.content[0].text
    else:
        # Fallback: simple aggregation
        synthesis = "**Observations:**\n\n"
        for a in reversed(analyses):
            synthesis += f"- Task #{a.task_id}: {a.analysis[:100]}\n"

    return {
        "success": True,
        "summary": f"Synthesized {len(analyses)} observations",
        "detail": synthesis,
    }


async def _tool_sleep_mode(inp: dict) -> dict:
    enabled = inp.get("enabled", True)
    payload = json.dumps({"type": "sleep_mode", "enabled": enabled})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, payload)
    state = "sleep" if enabled else "wake"
    return {
        "success": True,
        "summary": f"Board {state} command sent",
        "detail": f"**Board is now {'sleeping' if enabled else 'awake'}.**",
    }


async def _tool_delete_schedule(inp: dict) -> dict:
    from app.db.database import async_session
    from app.scheduler.service import get_schedule, delete_schedule as svc_delete
    from app.scheduler.notify import notify_schedule_update

    schedule_id = inp.get("schedule_id")
    if not schedule_id:
        return {"success": False, "summary": "schedule_id required", "detail": ""}

    async with async_session() as db:
        try:
            schedule = await get_schedule(db, int(schedule_id))
            name = schedule.name
            await svc_delete(db, int(schedule_id))
        except Exception as e:
            return {"success": False, "summary": f"Delete failed: {e}", "detail": ""}

    payload = json.dumps({"type": "delete_schedule", "schedule_id": schedule_id})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, payload)
    await notify_schedule_update()

    return {
        "success": True,
        "summary": f"Deleted: {name}",
        "detail": f"**Schedule deleted:** \"{name}\"",
    }


async def _tool_delete_image(inp: dict) -> dict:
    from pathlib import Path
    from sqlalchemy import delete as sql_delete
    from app.analysis.models import AnalysisResult
    from app.db.database import async_session

    date = inp.get("date", "")
    filename = inp.get("filename", "")

    if not date or not filename:
        return {"success": False, "summary": "date and filename required", "detail": ""}

    # Prevent path traversal
    if any(c in date + filename for c in ("/", "\\", "..")):
        return {"success": False, "summary": "Invalid path characters", "detail": ""}

    img_path = Path(settings.upload_dir) / date / filename
    if not img_path.exists():
        return {"success": False, "summary": f"{filename} not found", "detail": f"Image `{filename}` not found in {date}/"}

    img_path.unlink()

    # Delete matching AnalysisResult row (filename: task_{id}_{ts}.jpg)
    parts = Path(filename).stem.split("_")
    if len(parts) >= 2:
        try:
            task_id = int(parts[1])
            async with async_session() as db:
                await db.execute(sql_delete(AnalysisResult).where(AnalysisResult.task_id == task_id))
                await db.commit()
        except (ValueError, IndexError):
            pass

    return {
        "success": True,
        "summary": f"Deleted {filename}",
        "detail": f"**Image deleted:** `{filename}` ({date})",
    }


async def _tool_list_schedules() -> dict:
    """Return all schedules (active and inactive) — defense-in-depth alongside
    the ambient world-state snapshot, for when the agent needs to dig deeper
    than the compact summary (e.g. an older/inactive schedule)."""
    from app.db.database import async_session
    from app.scheduler.service import list_schedules as _list_schedules

    async with async_session() as db:
        schedules = await _list_schedules(db)

    if not schedules:
        return {
            "success": True,
            "summary": "No schedules found",
            "detail": "No schedules exist yet. Create one with a monitoring request.",
        }

    lines = ["**Schedules:**\n"]
    for s in schedules:
        times = [t.time for t in s.tasks]
        time_range = f"{times[0]}–{times[-1]}" if len(times) > 1 else (times[0] if times else "—")
        status = "ACTIVE" if s.is_active else "inactive"
        lines.append(f"- **#{s.id}** \"{s.name}\" — {status}, {len(s.tasks)} task(s), {time_range}")

    return {
        "success": True,
        "summary": f"{len(schedules)} schedule(s)",
        "detail": "\n".join(lines),
    }


# ── Fallback: rule-based dispatch when no API key ────────

def _fallback_model_key() -> str:
    """Best available analysis backend when the chat planner itself is
    degraded (no openrouter_api_key). Mirrors the resolution used elsewhere
    in this file (agent_chat's schedule-planning call sites)."""
    if settings.openrouter_api_key:
        return "openrouter"
    if settings.anthropic_api_key:
        return "claude-sonnet"
    return "qwen3-vl"


async def _fallback_dispatch(message: str, session_id: str):
    """Simple keyword matching when no chat-planning model is configured.

    Capture intents route through _capture_pipeline/_capture_sequence_pipeline
    (not the bare _tool_capture_now/_tool_capture_sequence helpers) so images
    still stream back to the dashboard as inline thumbnails — see the
    2026-08-19 fix: _tool_capture_now fires the MQTT command and returns
    immediately with no image_url, which is why captures triggered via chat
    never appeared in the UI even though they physically uploaded fine.
    """
    msg = message.lower()

    is_single_capture = any(
        w in msg for w in [
            "take a picture", "capture now", "snap", "photo now",
            "what do you see", "what does", "look at", "check on", "what's there",
        ]
    )
    is_sequence_capture = any(
        w in msg for w in ["burst", "sequence", "multiple", "several pictures"]
    )

    if is_single_capture or is_sequence_capture:
        model_key = _fallback_model_key()
        bench_run_id = _timing.new_run_id()
        bench_task_id = next_task_id()

        if is_sequence_capture:
            count = 3
            for m in re.findall(r"(\d+)", message):
                count = int(m)
                break
            async for ev in _capture_sequence_pipeline(
                {"count": count, "interval_ms": 2000},
                bench_run_id=bench_run_id,
                model_key=model_key,
            ):
                yield ev
        else:
            async for ev in _capture_pipeline(
                {}, model_key,
                bench_task_id=bench_task_id, bench_run_id=bench_run_id,
            ):
                yield ev
        return

    if any(w in msg for w in ["analyze", "latest image", "last capture", "previous analysis", "last analysis"]):
        result = await _tool_analyze_latest()
    elif any(w in msg for w in ["ping", "alive", "responsive"]):
        result = await _tool_ping()
    elif any(w in msg for w in ["status", "online", "health", "uptime"]):
        result = await _tool_board_status()
    elif any(w in msg for w in ["activate schedule", "start schedule", "run schedule", "resume schedule"]):
        ids = re.findall(r"\d+", message)
        if ids:
            result = await _tool_activate_schedule({"schedule_id": int(ids[0])})
        else:
            result = {"success": False, "summary": "Schedule ID required", "detail": "Please specify which schedule to activate (e.g. 'activate schedule 3')."}
    elif any(w in msg for w in ["stop", "cancel", "deactivate", "disable", "turn off"]):
        result = await _tool_deactivate_schedule({})
    elif any(w in msg for w in ["change schedule", "update schedule", "modify schedule", "reschedule", "adjust schedule"]):
        result = await _tool_modify_schedule({"prompt": message})
    elif any(w in msg for w in ["delete schedule", "remove schedule"]):
        # best-effort: extract numeric id from message
        ids = re.findall(r"\d+", message)
        if ids:
            result = await _tool_delete_schedule({"schedule_id": int(ids[0])})
        else:
            result = {"success": False, "summary": "Schedule ID required", "detail": "Please specify which schedule to delete (e.g. 'delete schedule 3')."}
    elif any(w in msg for w in ["sleep", "standby"]):
        result = await _tool_sleep_mode({"enabled": True})
    elif any(w in msg for w in ["wake up", "wake the board"]):
        result = await _tool_sleep_mode({"enabled": False})
    elif any(w in msg for w in ["monitor", "schedule", "every", "between", "watch"]):
        result = await _tool_create_schedule({"prompt": message})
    else:
        result = {
            "success": True,
            "summary": "Help",
            "detail": (
                "I can: **schedule monitoring**, **capture images**, **analyze captures**, "
                "**ping the board**, or **check status**. Try a natural language request!"
            ),
        }

    yield _sse_event("tool_call", {"id": "action", "label": "Processing..."})
    yield _sse_event("tool_result", {
        "id": "action",
        "success": result["success"],
        "summary": result["summary"],
    })
    yield _sse_event("reply", {"text": result.get("detail", "")})
