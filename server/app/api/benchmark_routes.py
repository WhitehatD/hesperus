"""
Thesis IoT Server — Benchmark Endpoints

Provides three benchmark surfaces:
  GET  /api/benchmark/latency   — Query CaptureLatency rows (stage-level timestamps)
  POST /api/benchmark/plan      — Single planning pass: prompt + model → tool_call result
  POST /api/benchmark/analyze   — Single analysis pass: image + model → analysis result

No SSE, no MQTT, no board interaction. Safe to call from offline benchmark runners.
"""

import io
import os
import tempfile
import time

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile
from sqlalchemy import delete, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.database import get_db
from app.analysis.models import CaptureLatency, EnergyTelemetry
from app.api.agent_routes import AGENT_TOOLS, AGENT_SYSTEM_PROMPT
from app.analysis.engine import analyze_image
from app.config import settings
from app.planning import engine as planning_engine

router = APIRouter(prefix="/benchmark", tags=["benchmark"])


# ── Latency query ────────────────────────────────────────────────────────────

@router.get("/latency")
async def list_latency(
    limit: int = 200,
    model_key: str | None = None,
    db: AsyncSession = Depends(get_db),
):
    """Return raw CaptureLatency rows, newest first.

    Query params:
      limit     — max rows returned (default 200)
      model_key — optional filter by model (e.g. claude-haiku)
    """
    q = select(CaptureLatency).order_by(CaptureLatency.id.desc()).limit(limit)
    if model_key:
        q = q.where(CaptureLatency.model_key == model_key)
    rows = (await db.execute(q)).scalars().all()
    return {
        "rows": [
            {c.name: getattr(r, c.name) for c in CaptureLatency.__table__.columns}
            for r in rows
        ]
    }


# ── Energy telemetry (RQ3 measured duty cycle) ─────────────────────────────────

@router.get("/energy")
async def list_energy(db: AsyncSession = Depends(get_db)):
    """Aggregate + raw energy phase-timer telemetry from the board.

    The firmware publishes ``{"status":"energy","window_ms","ps_rest_ms",
    "capture_ms"}`` once per minute while in WIFI_PS_REST; on_message persists
    each as an EnergyTelemetry row. This endpoint returns the per-window rows
    plus aggregates used to compute the MEASURED duty cycle for RQ3. Feed the
    JSON to ``scripts/energy_model.py --measured-json``.
    """
    rows = (await db.execute(
        select(EnergyTelemetry).order_by(EnergyTelemetry.id)
    )).scalars().all()
    total_window = sum(r.window_ms for r in rows)
    total_rest = sum(r.ps_rest_ms for r in rows)
    total_capture = sum(r.capture_ms for r in rows)
    total_active = max(total_window - total_rest, 0)
    return {
        "windows": len(rows),
        "total_window_ms": total_window,
        "total_ps_rest_ms": total_rest,
        "total_capture_ms": total_capture,
        "total_active_ms": total_active,
        "measured_active_fraction": (total_active / total_window) if total_window else None,
        "measured_capture_fraction": (total_capture / total_window) if total_window else None,
        "rows": [
            {
                "window_ms": r.window_ms,
                "ps_rest_ms": r.ps_rest_ms,
                "capture_ms": r.capture_ms,
                "received_at": r.received_at.isoformat() if r.received_at else None,
            }
            for r in rows
        ],
    }


@router.post("/energy/reset")
async def reset_energy(db: AsyncSession = Depends(get_db)):
    """Clear all energy phase-timer rows to start a clean RQ3 measurement run.

    The dashboard shows the CUMULATIVE duty split across all stored windows, so
    stale or invalid windows (e.g. captured before a firmware fix, where
    ps_rest_ms was misreported) pollute the average. The operator resets before
    a fresh run so the measured duty cycle reflects only valid windows.
    """
    result = await db.execute(delete(EnergyTelemetry))
    await db.commit()
    return {"status": "reset", "deleted": int(result.rowcount or 0)}


# ── Planning benchmark ───────────────────────────────────────────────────────

@router.post("/plan")
async def benchmark_plan(payload: dict):
    """Run a single planning pass and return the tool_call result.

    No SSE, no MQTT, no board interaction.

    Request body:
      {
        "prompt": "Take a picture every 30 minutes for 2 hours",
        "model_key": "claude-haiku" | "claude-sonnet" | "gemini-3-flash-preview" | ...
      }

    Response:
      {
        "model_key": str,
        "tool_name": str | null,
        "tool_input": dict | null,
        "raw_text": str,
        "latency_ms": float,
        "success": bool,
        "error": str | null
      }
    """
    prompt = payload.get("prompt", "")
    model_key = payload.get("model_key", "claude-haiku")

    if not prompt:
        raise HTTPException(status_code=422, detail="prompt is required")

    t0 = time.time()

    # Route to the appropriate backend
    try:
        if model_key in ("claude-haiku", "claude-sonnet"):
            result = await _plan_with_claude(prompt, model_key)
        elif model_key == "claude-sonnet-nothink":
            # Control arm: Sonnet at T=0.1 without extended thinking. Lets us
            # separate "Sonnet variance under T=1.0 thinking" from "Sonnet
            # baseline variance" — necessary for an honest cross-backend
            # variance comparison.
            result = await _plan_with_claude_nothink(prompt)
        elif model_key in ("gemini-3", "gemini-3-flash-preview"):
            result = await _plan_with_gemini(prompt)
        elif model_key == "qwen3-vl":
            # Qwen3-VL-30B supports OpenAI-format tool_calls via llama.cpp --jinja default
            # template (empirically verified 2026-05-28: 9/10 correct routing on full agent
            # palette). Qwen2.5-VL-3B does NOT support tool_use cleanly and is excluded.
            result = await _plan_with_qwen(prompt)
        else:
            raise HTTPException(
                status_code=422,
                detail=f"Unsupported model_key for planning: {model_key}. "
                       "Supported: claude-haiku, claude-sonnet, "
                       "claude-sonnet-nothink, gemini-3, gemini-3-flash-preview, "
                       "qwen3-vl",
            )
    except HTTPException:
        raise
    except Exception as e:
        return {
            "model_key": model_key,
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "latency_ms": (time.time() - t0) * 1000,
            "success": False,
            "error": f"{type(e).__name__}: {e}",
        }

    result["latency_ms"] = (time.time() - t0) * 1000
    result["model_key"] = model_key
    return result


def _empty_chain_result(reason: str = "No create_schedule tool_call to chain") -> dict:
    """Default values for the chained-engine fields when no chain runs."""
    return {
        "schedule_json": None,
        "schedule_engine_ms": None,
        "schedule_error": reason,
    }


async def _chain_to_planning_engine(forwarded_prompt: str, agent_model_key: str) -> dict:
    """After the agent emits a successful create_schedule tool_call, chain to the
    planning engine using the SAME backend model and capture the actual generated
    schedule JSON. Mandatory for programmatic plan-quality scoring (interval
    accuracy, t=0 preservation, count, span).

    The agent's model_key may differ from what the planning engine accepts —
    notably "gemini-3-flash-preview" must be normalised to "gemini-3" because
    the engine's CLAUDE_MODEL_MAP doesn't know the cloud variant tag. Other
    keys pass through unchanged.

    Returns: {schedule_json, schedule_engine_ms, schedule_error}.
    """
    planner_key = "gemini-3" if agent_model_key.startswith("gemini") else agent_model_key

    t0 = time.monotonic()
    try:
        plan = await planning_engine.generate_plan(
            forwarded_prompt, model_key=planner_key
        )
        # plan.tasks is a list of ScheduledTask pydantic models
        schedule_json = []
        for task in plan.tasks:
            if hasattr(task, "model_dump"):
                schedule_json.append(task.model_dump())
            elif hasattr(task, "dict"):
                schedule_json.append(task.dict())
            else:
                schedule_json.append(dict(task))
        return {
            "schedule_json": schedule_json,
            "schedule_engine_ms": round((time.monotonic() - t0) * 1000, 1),
            "schedule_error": None,
        }
    except Exception as e:
        return {
            "schedule_json": None,
            "schedule_engine_ms": round((time.monotonic() - t0) * 1000, 1),
            "schedule_error": f"{type(e).__name__}: {e}",
        }


async def _plan_with_claude(prompt: str, model_key: str) -> dict:
    """Call Claude with the agent tools, capture reasoning + tokens, then chain
    to the planning engine on a successful create_schedule call.

    Extended thinking is enabled when model_key == "claude-sonnet" (Anthropic
    extended thinking is currently Sonnet-only). Anthropic requires
    temperature=1.0 in thinking mode; the benchmark accounts for this by
    repeating each (prompt, model) pair multiple times via --reps.
    """
    import anthropic

    if not settings.anthropic_api_key:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": "ANTHROPIC_API_KEY not configured",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result("ANTHROPIC_API_KEY not configured"),
        }

    CLAUDE_MODEL_MAP = {
        "claude-haiku": settings.claude_haiku_model,
        "claude-sonnet": settings.claude_sonnet_model,
    }
    model = CLAUDE_MODEL_MAP.get(model_key, settings.claude_haiku_model)
    use_thinking = (model_key == "claude-sonnet")

    create_kwargs: dict = {
        "model": model,
        "max_tokens": 4096 if use_thinking else 1024,
        "system": AGENT_SYSTEM_PROMPT,
        "tools": AGENT_TOOLS,
        "messages": [{"role": "user", "content": prompt}],
    }
    if use_thinking:
        create_kwargs["thinking"] = {"type": "enabled", "budget_tokens": 2000}
        create_kwargs["temperature"] = 1.0
    else:
        create_kwargs["temperature"] = 0.1

    client = anthropic.AsyncAnthropic(api_key=settings.anthropic_api_key)
    try:
        response = await client.messages.create(**create_kwargs)
    except Exception as e:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": f"{type(e).__name__}: {e}",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result(f"upstream error: {type(e).__name__}"),
        }

    tool_name = None
    tool_input = None
    raw_text_parts: list[str] = []
    thinking_parts: list[str] = []

    for block in response.content:
        btype = getattr(block, "type", None)
        if btype == "thinking":
            thinking_parts.append(getattr(block, "thinking", "") or "")
        elif btype == "text":
            raw_text_parts.append(getattr(block, "text", "") or "")
        elif btype == "tool_use":
            tool_name = block.name
            tool_input = block.input

    thinking_text = "".join(thinking_parts)
    result = {
        "tool_name": tool_name,
        "tool_input": tool_input,
        "raw_text": "".join(raw_text_parts),
        "thinking_text": thinking_text,
        "success": tool_name is not None,
        "error": None if tool_name is not None else "No tool_use block in response",
        "input_tokens": response.usage.input_tokens,
        "output_tokens": response.usage.output_tokens,
        # Anthropic doesn't expose a separate "thinking_tokens" counter — use
        # character length as a proxy that's comparable across reps.
        "thinking_tokens": len(thinking_text) if thinking_text else 0,
    }

    if tool_name == "create_schedule" and isinstance(tool_input, dict) and tool_input.get("prompt"):
        result.update(await _chain_to_planning_engine(tool_input["prompt"], model_key))
    else:
        result.update(_empty_chain_result())

    return result


async def _plan_with_claude_nothink(prompt: str) -> dict:
    """Sonnet planning at T=0.1 WITHOUT extended thinking — control arm.

    This is a thin wrapper that calls the normal _plan_with_claude but with
    model_key="claude-sonnet" overridden to skip the thinking branch. Used to
    measure baseline Sonnet variance for fair comparison against Haiku/Gemini
    which also run at T=0.1.

    Implementation: temporarily override use_thinking by calling the function
    with model_key="claude-haiku" — NO, that would route to the wrong model.
    Easier: inline a stripped-down version.
    """
    import anthropic

    if not settings.anthropic_api_key:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": "ANTHROPIC_API_KEY not configured",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result("ANTHROPIC_API_KEY not configured"),
        }

    client = anthropic.AsyncAnthropic(api_key=settings.anthropic_api_key)
    try:
        response = await client.messages.create(
            model=settings.claude_sonnet_model,
            max_tokens=1024,
            system=AGENT_SYSTEM_PROMPT,
            tools=AGENT_TOOLS,
            messages=[{"role": "user", "content": prompt}],
            temperature=0.1,
        )
    except Exception as e:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": f"{type(e).__name__}: {e}",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result(f"upstream error: {type(e).__name__}"),
        }

    tool_name = None
    tool_input = None
    raw_text_parts: list[str] = []
    for block in response.content:
        btype = getattr(block, "type", None)
        if btype == "text":
            raw_text_parts.append(getattr(block, "text", "") or "")
        elif btype == "tool_use":
            tool_name = block.name
            tool_input = block.input

    result = {
        "tool_name": tool_name,
        "tool_input": tool_input,
        "raw_text": "".join(raw_text_parts),
        "thinking_text": "",  # intentionally empty — this arm has thinking off
        "success": tool_name is not None,
        "error": None if tool_name is not None else "No tool_use block",
        "input_tokens": response.usage.input_tokens,
        "output_tokens": response.usage.output_tokens,
        "thinking_tokens": 0,
    }

    # Chain to planning engine using regular claude-sonnet (no thinking there
    # either — let the planner's normal config decide).
    if tool_name == "create_schedule" and isinstance(tool_input, dict) and tool_input.get("prompt"):
        result.update(await _chain_to_planning_engine(tool_input["prompt"], "claude-sonnet"))
    else:
        result.update(_empty_chain_result())

    return result


def _anthropic_tools_to_openai(tools: list) -> list:
    """Convert Anthropic-format tool schema to OpenAI function-call format.

    llama.cpp's /v1/chat/completions endpoint expects OpenAI-style:
        [{"type": "function", "function": {"name", "description", "parameters"}}]
    Whereas AGENT_TOOLS is in Anthropic-style:
        [{"name", "description", "input_schema"}]
    """
    return [
        {
            "type": "function",
            "function": {
                "name": t["name"],
                "description": t.get("description", ""),
                "parameters": t.get("input_schema", {}),
            },
        }
        for t in tools
    ]


async def _plan_with_qwen(prompt: str) -> dict:
    """Call Qwen3-VL-30B via llama.cpp's OpenAI-compatible API with tool_use enabled.

    Uses the SAME AGENT_TOOLS and AGENT_SYSTEM_PROMPT as the Claude path so the
    planning benchmark is an apples-to-apples comparison: same tool palette,
    same routing instructions, only the model differs.

    Empirically verified (2026-05-28) that llama.cpp b9222 with --jinja correctly
    parses Qwen3-VL's <tool_call> output blocks into OpenAI tool_calls arrays.
    The Qwen2.5-VL-3B variant does NOT support tool_use cleanly under the same
    setup and is excluded from planning evaluation.
    """
    from openai import AsyncOpenAI

    if not settings.vllm_base_url:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": "VLLM_BASE_URL not configured",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result("VLLM_BASE_URL not configured"),
        }

    openai_tools = _anthropic_tools_to_openai(AGENT_TOOLS)

    client = AsyncOpenAI(
        base_url=settings.vllm_base_url,
        api_key="not-needed",
    )

    try:
        response = await client.chat.completions.create(
            model=settings.vllm_model,
            messages=[
                {"role": "system", "content": AGENT_SYSTEM_PROMPT},
                {"role": "user", "content": prompt},
            ],
            tools=openai_tools,
            temperature=0.1,
            max_tokens=1024,
        )
    except Exception as e:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": f"{type(e).__name__}: {e}",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result(f"upstream error: {type(e).__name__}"),
        }

    msg = response.choices[0].message
    tool_calls = msg.tool_calls or []
    raw_text = msg.content or ""

    tool_name = None
    tool_input = None
    if tool_calls:
        tc = tool_calls[0]
        tool_name = tc.function.name
        # llama.cpp returns args as a JSON string; parse defensively
        import json as _json
        try:
            tool_input = _json.loads(tc.function.arguments) if tc.function.arguments else {}
        except _json.JSONDecodeError:
            tool_input = {"_raw_arguments": tc.function.arguments}

    usage = getattr(response, "usage", None)
    result = {
        "tool_name": tool_name,
        "tool_input": tool_input,
        "raw_text": raw_text,
        "thinking_text": "",  # Qwen3-VL via llama.cpp default jinja: no thinking exposed
        "success": tool_name is not None,
        "error": None if tool_name is not None else "No tool_calls in response",
        "input_tokens": getattr(usage, "prompt_tokens", None) if usage else None,
        "output_tokens": getattr(usage, "completion_tokens", None) if usage else None,
        "thinking_tokens": None,
    }

    if tool_name == "create_schedule" and isinstance(tool_input, dict) and tool_input.get("prompt"):
        result.update(await _chain_to_planning_engine(tool_input["prompt"], "qwen3-vl"))
    else:
        result.update(_empty_chain_result())

    return result


async def _plan_with_gemini(prompt: str) -> dict:
    """Call Gemini in tool-use mode and return extracted tool_call result.

    Gemini's function-calling API is used; tools are translated from the
    Anthropic tool schema format into Gemini FunctionDeclaration objects.
    """
    if not settings.gemini_api_key:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": "GEMINI_API_KEY not configured",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result("GEMINI_API_KEY not configured"),
        }

    try:
        from google import genai
        from google.genai import types
    except ImportError:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": "google-genai package not installed",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result("google-genai not installed"),
        }

    # Convert Anthropic tool schema → Gemini FunctionDeclaration
    gemini_tools = []
    for t in AGENT_TOOLS:
        gemini_tools.append(
            types.Tool(
                function_declarations=[
                    types.FunctionDeclaration(
                        name=t["name"],
                        description=t.get("description", ""),
                        parameters=t.get("input_schema", {}),
                    )
                ]
            )
        )

    client = genai.Client(api_key=settings.gemini_api_key)
    try:
        response = await client.aio.models.generate_content(
            model=settings.gemini_model,
            contents=[
                types.Content(
                    role="user",
                    parts=[types.Part(text=f"{AGENT_SYSTEM_PROMPT}\n\nUser request: {prompt}")],
                )
            ],
            config=types.GenerateContentConfig(tools=gemini_tools, temperature=0.1),
        )
    except Exception as e:
        return {
            "tool_name": None,
            "tool_input": None,
            "raw_text": "",
            "thinking_text": "",
            "success": False,
            "error": f"{type(e).__name__}: {e}",
            "input_tokens": None,
            "output_tokens": None,
            "thinking_tokens": None,
            **_empty_chain_result(f"upstream error: {type(e).__name__}"),
        }

    tool_name = None
    tool_input = None
    raw_text = ""

    for candidate in response.candidates or []:
        for part in (candidate.content.parts or []):
            if part.text:
                raw_text += part.text
            if part.function_call:
                tool_name = part.function_call.name
                # Gemini returns a MapComposite — convert to plain dict
                tool_input = dict(part.function_call.args) if part.function_call.args else {}

    meta = getattr(response, "usage_metadata", None)
    result = {
        "tool_name": tool_name,
        "tool_input": tool_input,
        "raw_text": raw_text,
        "thinking_text": "",  # Gemini's thought_signature is opaque; not captured here
        "success": tool_name is not None,
        "error": None if tool_name is not None else "No function_call in response",
        "input_tokens": getattr(meta, "prompt_token_count", None) if meta else None,
        "output_tokens": getattr(meta, "candidates_token_count", None) if meta else None,
        "thinking_tokens": None,
    }

    if tool_name == "create_schedule" and isinstance(tool_input, dict) and tool_input.get("prompt"):
        result.update(await _chain_to_planning_engine(tool_input["prompt"], "gemini-3"))
    else:
        result.update(_empty_chain_result())

    return result


# ── Analysis benchmark ───────────────────────────────────────────────────────

@router.post("/analyze")
async def benchmark_analyze(
    file: UploadFile = File(...),
    model_key: str = Form("claude-sonnet"),
    objective: str = Form("General visual inspection"),
    enable_thinking: bool = Form(True),
):
    """Run a single image analysis pass and return the result.

    Accepts a multipart file upload so the benchmark runner can send images
    from any machine without sharing a filesystem with the server.

    enable_thinking defaults to True for the benchmark surface: Sonnet runs
    with extended thinking enabled so the JSONL row captures the reasoning
    trace needed for semantic-quality commentary. Other backends ignore the
    flag (they don't expose thinking through their SDKs in a comparable form).

    No DB write — results are returned directly to the caller for offline
    scoring and aggregation by the benchmark runner.
    """
    from PIL import Image

    t0 = time.time()
    tmp_path: str | None = None
    try:
        # Normalize every benchmark image to baseline JPEG/RGB so all backends
        # see the same input. Anthropic Vision rejects palette-mode (mode='P')
        # and CMYK; Gemini is more lenient. Normalizing at the server boundary
        # removes dataset-format coupling from the runner and from comparisons.
        raw = await file.read()
        with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tmp:
            tmp_path = tmp.name
            with Image.open(io.BytesIO(raw)) as im:
                if im.mode != "RGB":
                    im = im.convert("RGB")
                im.save(tmp_path, format="JPEG", quality=92)

        # claude-sonnet-nothink is the same model as claude-sonnet but with
        # extended thinking forcibly disabled — used as a control arm to
        # separate "Sonnet variance under T=1.0 thinking" from "Sonnet variance
        # at the standard T=0.1 baseline". See methodology section of thesis.
        if model_key == "claude-sonnet-nothink":
            actual_model_key = "claude-sonnet"
            actual_thinking = False
        else:
            actual_model_key = model_key
            actual_thinking = enable_thinking

        result = await analyze_image(
            tmp_path, objective, actual_model_key, enable_thinking=actual_thinking
        )
        return {
            "model_key": model_key,  # report the original key in the row
            "objective": objective,
            "image_name": file.filename,
            "latency_ms": (time.time() - t0) * 1000,
            "result": result,
            "success": True,
            "error": None,
        }
    except ValueError as e:
        raise HTTPException(status_code=422, detail=str(e))
    except Exception as e:
        return {
            "model_key": model_key,
            "objective": objective,
            "image_name": file.filename,
            "latency_ms": (time.time() - t0) * 1000,
            "result": None,
            "success": False,
            "error": f"{type(e).__name__}: {e}",
        }
    finally:
        if tmp_path and os.path.exists(tmp_path):
            os.unlink(tmp_path)
