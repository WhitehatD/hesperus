"""
Thesis IoT Server — AI Planning Engine
Core innovation: Natural Language → JSON Hardware Schedule.

Primary backend: Claude Sonnet (Anthropic API)
Legacy backends (for thesis benchmarking):
  - Qwen3-VL-30B-A3B (via vLLM, OpenAI-compatible)
  - Gemini 3 Flash (via Google GenAI API)
"""

import asyncio
import json
from datetime import datetime

import anthropic
from openai import APIConnectionError, APIStatusError, APITimeoutError, RateLimitError

from app.config import settings
from app.api.schemas import PlanResponse, ScheduledTask

# See matching note in app/analysis/engine.py — same OpenRouter gateway,
# same observed transient-failure profile.
_OPENROUTER_TRANSIENT_ERRORS = (
    ValueError,  # our own _extract_completion_text/_parse_schedule guards
    APIConnectionError,
    APITimeoutError,
    RateLimitError,
    APIStatusError,
)


# ── System Prompt ────────────────────────────────────────

PLANNING_SYSTEM_PROMPT = """You are an AI Planning Engine for an IoT visual monitoring system.

Convert a natural language monitoring request into a JSON schedule for an STM32 camera board.

The request will include the current time. NEVER schedule tasks in the past.

Output ONLY valid JSON:
{
  "tasks": [
    {
      "time": "HH:MM",
      "action": "CAPTURE_IMAGE",
      "id": <sequential_integer>,
      "objective": "<what to analyze>"
    }
  ]
}

Frequency guidelines — capture frequently for useful monitoring:
- 2-5 minutes: every 1 minute
- 5-15 minutes: every 2 minutes
- 15-30 minutes: every 3 minutes
- 30-60 minutes: every 5 minutes
- 1-2 hours: every 10 minutes
- 2-4 hours: every 15 minutes
- 4-8 hours: every 30 minutes
- Overnight/24h: every 1 hour

Rules:
- Tasks must be at least 1 minute apart (board needs time to capture + upload)
- Start from the current time (or specified start), never earlier
- If duration is given without start time, start NOW (current time)
- If no duration given, default to 30 minutes
- Each task's objective should be specific to what the user wants monitored
- For evolving observations, vary the objective slightly (e.g., "check for changes since last capture")
- Task IDs start at 1 and increment
- Output ONLY JSON, no explanation"""


# ── Planning Engine ──────────────────────────────────────

_plan_counter = 0


async def generate_plan(prompt: str, model_key: str = "claude-sonnet") -> PlanResponse:
    """
    Generate a task schedule from a natural language prompt.

    Args:
        prompt: User's natural language monitoring request.
        model_key: Which AI backend to use.

    Returns:
        PlanResponse with the generated schedule.
    """
    global _plan_counter
    _plan_counter += 1

    if model_key == "claude-sonnet":
        schedule = await _plan_with_claude(prompt, settings.claude_sonnet_model)
    elif model_key == "claude-haiku":
        schedule = await _plan_with_claude(prompt, settings.claude_haiku_model)
    elif model_key in ("qwen3-vl", "qwen2.5-vl"):
        schedule = await _plan_with_vllm(prompt, model_key)
    elif model_key == "gemini-3":
        schedule = await _plan_with_gemini(prompt)
    elif model_key == "openrouter":
        schedule = await _plan_with_openrouter_retrying(prompt)
    else:
        raise ValueError(f"Unknown model: {model_key}")

    return PlanResponse(
        plan_id=_plan_counter,
        prompt=prompt,
        tasks=schedule,
        model_used=model_key,
        created_at=datetime.now(),
    )


async def _plan_with_claude(prompt: str, model: str) -> list[ScheduledTask]:
    """Generate schedule using Claude (Anthropic API)."""
    client = anthropic.AsyncAnthropic(api_key=settings.anthropic_api_key)

    response = await client.messages.create(
        model=model,
        max_tokens=2048,
        system=PLANNING_SYSTEM_PROMPT,
        messages=[{"role": "user", "content": prompt}],
        temperature=0.1,
        timeout=30.0,
    )

    raw_text = response.content[0].text
    try:
        return _parse_schedule(raw_text)
    except ValueError:
        # Retry once with a stricter JSON instruction
        strict_prompt = (
            prompt
            + "\n\nReturn ONLY valid JSON with no prose. "
            "Keys: tasks[] with id, time, action, objective, repeat."
        )
        response2 = await client.messages.create(
            model=model,
            max_tokens=2048,
            system=PLANNING_SYSTEM_PROMPT,
            messages=[{"role": "user", "content": strict_prompt}],
            temperature=0.0,
            timeout=30.0,
        )
        raw_text2 = response2.content[0].text
        try:
            return _parse_schedule(raw_text2)
        except ValueError as exc2:
            raise ValueError(
                f"Schedule planning failed: LLM returned malformed JSON after retry. "
                f"Raw: {raw_text2[:200]}"
            ) from exc2


async def _plan_with_vllm(prompt: str, model_key: str) -> list[ScheduledTask]:
    """Generate schedule using local vLLM / llama.cpp (OpenAI-compatible API).

    Routes by model_key to the correct backend URL/model. Mirrors the
    analysis-engine routing pattern so qwen3-vl hits port 8001 and qwen2.5-vl
    hits port 8002.
    """
    from openai import AsyncOpenAI

    if model_key == "qwen3-vl":
        base_url = settings.vllm_base_url
        model_name = settings.vllm_model
    elif model_key == "qwen2.5-vl":
        base_url = settings.vllm_qwen25_base_url
        model_name = settings.vllm_qwen25_model
    else:
        raise ValueError(f"Unknown vllm model_key for planning: {model_key}")

    client = AsyncOpenAI(
        base_url=base_url,
        api_key="not-needed",
    )

    response = await client.chat.completions.create(
        model=model_name,
        messages=[
            {"role": "system", "content": PLANNING_SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ],
        temperature=0.1,
        max_tokens=2048,
    )

    return _parse_schedule(_extract_completion_text(response, backend_label=f"vLLM ({model_key})"))


async def _plan_with_openrouter(prompt: str) -> list[ScheduledTask]:
    """Generate schedule via OpenRouter. Production default as of 2026-08-19
    — uses openrouter_planner_model (cheap, tools-capable, text-only; schedule
    generation never reasons about images)."""
    from openai import AsyncOpenAI

    client = AsyncOpenAI(
        base_url=settings.openrouter_base_url,
        api_key=settings.openrouter_api_key,
    )

    response = await client.chat.completions.create(
        model=settings.openrouter_planner_model,
        messages=[
            {"role": "system", "content": PLANNING_SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ],
        temperature=0.1,
        max_tokens=2048,
    )

    return _parse_schedule(_extract_completion_text(response, backend_label="OpenRouter"))


async def _plan_with_openrouter_retrying(
    prompt: str, max_attempts: int = 3
) -> list[ScheduledTask]:
    """See matching _analyze_with_openrouter_retrying in analysis/engine.py:
    retry transient OpenRouter failures. No cross-backend fallback — same
    reasoning as the analysis engine (misreporting which backend actually
    planned, benchmark integrity)."""
    last_exc: Exception | None = None
    for attempt in range(max_attempts):
        try:
            return await _plan_with_openrouter(prompt)
        except _OPENROUTER_TRANSIENT_ERRORS as exc:
            last_exc = exc
            if attempt < max_attempts - 1:
                await asyncio.sleep(0.5 * (attempt + 1))

    assert last_exc is not None
    raise last_exc


async def _plan_with_gemini(prompt: str) -> list[ScheduledTask]:
    """Generate schedule using Gemini 3 Flash API."""
    from google import genai

    client = genai.Client(api_key=settings.gemini_api_key)

    response = await client.aio.models.generate_content(
        model=settings.gemini_model,
        contents=f"{PLANNING_SYSTEM_PROMPT}\n\nUser request: {prompt}",
    )

    if response.text is None:
        finish_reason = None
        candidates = getattr(response, "candidates", None) or []
        if candidates:
            finish_reason = getattr(candidates[0], "finish_reason", None)
        raise ValueError(
            f"Gemini returned no text content (finish_reason={finish_reason}). "
            "This usually means the response was blocked by a safety filter "
            "or the API returned zero candidates."
        )

    return _parse_schedule(response.text)


def _extract_completion_text(response, backend_label: str) -> str:
    """Extract message content from an OpenAI-compatible chat completion
    response, raising a clear diagnostic error instead of letting a bare
    ``None`` content silently propagate to ``_parse_schedule`` (which would
    crash with an opaque ``AttributeError: 'NoneType' object has no
    attribute 'strip'``).

    Mirrors ``app.analysis.engine._extract_completion_text`` — kept as a
    separate local copy rather than a shared import so the planning and
    analysis engines stay independently deployable/testable, matching this
    file's existing pattern of duplicating the OpenAI-compatible call shape
    rather than sharing it with the analysis engine.
    """
    if not response.choices:
        raise ValueError(f"{backend_label} returned zero choices — empty response body")

    choice = response.choices[0]
    content = choice.message.content
    finish_reason = getattr(choice, "finish_reason", None)

    if content is None or not content.strip():
        reasoning = getattr(choice.message, "reasoning", None) or getattr(
            choice.message, "reasoning_content", None
        )
        reasoning_hint = " (reasoning trace present but no final content — likely truncated by max_tokens)" if reasoning else ""
        raise ValueError(
            f"{backend_label} returned empty content (finish_reason={finish_reason})"
            f"{reasoning_hint}. This usually means the upstream provider hit an "
            "error, rate limit, or content filter, or the model exhausted "
            "max_tokens before producing an answer."
        )

    return content


def _parse_schedule(raw_output: str | None) -> list[ScheduledTask]:
    """Parse LLM output into a list of ScheduledTask objects."""
    if raw_output is None or not raw_output.strip():
        raise ValueError("LLM returned empty output — no content to parse as a schedule")

    text = raw_output.strip()
    # Strip markdown code fence if present
    if "```" in text:
        parts = text.split("```")
        # Take the content of the first code block
        if len(parts) >= 2:
            text = parts[1].lstrip("json").strip()

    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"LLM returned non-JSON output: {exc}") from exc

    tasks = data.get("tasks", [])
    result = []
    for t in tasks:
        try:
            result.append(ScheduledTask(
                id=int(t["id"]),
                time=t["time"],
                action=t.get("action", "CAPTURE_IMAGE"),
                objective=t.get("objective", ""),
                repeat=bool(t.get("repeat", False)),
            ))
        except KeyError as exc:
            raise ValueError(f"Schedule task missing required field: {exc}") from exc
    return result
