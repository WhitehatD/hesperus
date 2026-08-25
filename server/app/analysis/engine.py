"""
Thesis IoT Server — Multimodal Visual Analysis Engine

Core agentic layer: takes a captured image + the original monitoring objective
and produces a structured analysis with actionable recommendations.

Thesis multi-backend benchmark — all backends equal weight in evaluation:
  - Claude Sonnet / Haiku (Anthropic API — native vision)
  - Gemini 3 Flash (Google GenAI API)
  - Qwen3-VL-30B-A3B / Qwen2.5-VL-3B (local vLLM)
"""

import asyncio
import base64
import json
import time
from pathlib import Path

import anthropic
import httpx
from openai import APIConnectionError, APIStatusError, APITimeoutError, RateLimitError

from app.config import settings

# Transient failure modes observed from the OpenRouter gateway/upstream
# providers — production evidence (2026-08-18..24) shows these firing on
# 3 of 67 analyses with no pattern (not token-budget related: successful
# analyses peak at ~150 output tokens against a 1024-2048 cap), i.e. they
# are retryable soft-failures, not a systematic bug in the request itself.
_OPENROUTER_TRANSIENT_ERRORS = (
    RuntimeError,  # our own _extract_completion_text guard: empty/None content
    APIConnectionError,
    APITimeoutError,
    RateLimitError,
    APIStatusError,  # non-2xx from the gateway, incl. 5xx
)


# ── System Prompt ────────────────────────────────────────

ANALYSIS_SYSTEM_PROMPT = """You are the visual analysis engine of an autonomous IoT monitoring system.

You receive images captured by an STM32 microcontroller camera and the user's original monitoring objective.

Analyze the image and produce a JSON response with exactly these fields:
{
  "description": "<2-3 sentence factual description of what is visible in the image>",
  "findings": "<specific observations related to the monitoring objective>",
  "recommendation": "<one actionable recommendation based on what you see>",
  "flagged": <true or false>,
  "flag_reason": "<short reason this capture is notable, empty string if flagged is false>"
}

Rules:
- Be specific and factual — describe what you actually see, not what you assume
- If the image is blurry, dark, or unreadable, say so honestly
- The recommendation should be practical and immediately actionable
- Output ONLY the JSON, no markdown fences, no explanation

Flagging criteria (this decides whether a human gets pulled in):
- flagged = true → the findings show something NOTABLE relative to the stated
  objective: the watched-for event/condition is present, something changed or
  moved compared to what the objective describes as normal, something looks
  wrong/unsafe/anomalous, or the capture itself failed in a way that defeats
  monitoring (fully black, fully blown out, lens obstructed).
- flagged = false → routine: nothing notable, matches the expected baseline,
  the watched-for condition is absent. Uneventful captures are the norm — do
  NOT flag just because an image contains objects or people if that is exactly
  what the objective describes as normal.
- flag_reason must be ONE short sentence (max ~15 words) naming the specific
  trigger, e.g. "package left on the doorstep" — never a restatement of the
  whole description. Use "" when flagged is false."""


async def analyze_image(
    image_path: str,
    objective: str,
    model_key: str = "claude-sonnet",
    enable_thinking: bool = False,
) -> dict:
    """
    Analyze a captured image against a monitoring objective.

    Args:
        image_path: Path to the JPEG image on disk.
        objective: The original monitoring objective from the user's plan.
        model_key: Which AI backend to use.
        enable_thinking: If True and the model supports it (claude-sonnet only),
            enable extended thinking mode and capture the thinking_text in the
            return dict. Production callers leave this False. The benchmark
            harness sets it True for Sonnet to capture the reasoning trace
            needed for semantic-quality commentary on analysis output.

    Returns:
        Dict with: description, findings, recommendation, model_used,
        inference_time_ms, input_tokens, output_tokens, thinking_text (empty
        unless thinking was enabled), thinking_tokens.
    """
    start = time.monotonic()

    if model_key in ("claude-sonnet", "claude-haiku"):
        model = (
            settings.claude_sonnet_model
            if model_key == "claude-sonnet"
            else settings.claude_haiku_model
        )
        # Anthropic extended thinking is currently Sonnet-only.
        use_thinking = enable_thinking and model_key == "claude-sonnet"
        result = await _analyze_with_claude(image_path, objective, model, use_thinking)
    elif model_key in ("qwen3-vl", "qwen2.5-vl"):
        result = await _analyze_with_vllm(image_path, objective, model_key)
    elif model_key == "gemini-3":
        result = await _analyze_with_gemini(image_path, objective)
    elif model_key == "openrouter":
        result = await _analyze_with_openrouter_retrying(image_path, objective)
    else:
        raise ValueError(f"Unknown model: {model_key}")

    elapsed_ms = (time.monotonic() - start) * 1000
    result["model_used"] = model_key
    result["inference_time_ms"] = round(elapsed_ms, 1)
    # Ensure metadata keys are always present so the benchmark JSONL schema
    # stays uniform across backends, even when a backend doesn't expose them.
    result.setdefault("input_tokens", None)
    result.setdefault("output_tokens", None)
    result.setdefault("thinking_text", "")
    result.setdefault("thinking_tokens", None)
    return result


async def _analyze_with_claude(
    image_path: str, objective: str, model: str, enable_thinking: bool = False
) -> dict:
    """Analyze image using Claude (Anthropic API — native vision).

    When enable_thinking is True (Sonnet only), the API call requests extended
    thinking with a 2000-token budget. Anthropic requires temperature=1.0 in
    that mode; this is intentional and the benchmark accounts for it by
    repeating each (image, model) pair multiple times.
    """
    image_b64 = _load_image_b64(image_path)

    client = anthropic.AsyncAnthropic(api_key=settings.anthropic_api_key)

    create_kwargs: dict = {
        "model": model,
        "max_tokens": 4096 if enable_thinking else 1024,
        "system": ANALYSIS_SYSTEM_PROMPT,
        "messages": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "image",
                        "source": {
                            "type": "base64",
                            "media_type": "image/jpeg",
                            "data": image_b64,
                        },
                    },
                    {
                        "type": "text",
                        "text": f"Monitoring objective: {objective}",
                    },
                ],
            }
        ],
        "timeout": 60.0,
    }
    if enable_thinking:
        create_kwargs["thinking"] = {"type": "enabled", "budget_tokens": 2000}
        create_kwargs["temperature"] = 1.0
    else:
        create_kwargs["temperature"] = 0.1

    response = await client.messages.create(**create_kwargs)

    # Iterate response blocks: extended-thinking responses contain ThinkingBlock
    # entries BEFORE the final TextBlock. Concatenate text only from TextBlocks.
    text_parts: list[str] = []
    thinking_parts: list[str] = []
    for block in response.content:
        btype = getattr(block, "type", None)
        if btype == "thinking":
            thinking_parts.append(getattr(block, "thinking", "") or "")
        elif btype == "text":
            text_parts.append(getattr(block, "text", "") or "")

    parsed = _parse_analysis("".join(text_parts))
    parsed["input_tokens"] = response.usage.input_tokens
    parsed["output_tokens"] = response.usage.output_tokens
    parsed["thinking_text"] = "".join(thinking_parts)
    # Anthropic doesn't expose a separate "thinking_tokens" counter — thinking
    # contributes to output_tokens; record the character length as a proxy.
    parsed["thinking_tokens"] = len(parsed["thinking_text"]) if parsed["thinking_text"] else 0
    return parsed


async def _analyze_with_vllm(
    image_path: str, objective: str, model_key: str
) -> dict:
    """Analyze image using local OpenAI-compatible vision backend.

    Routes by model_key to the correct backend URL/model. Each open-weight
    model is served by its own llama-server / vLLM instance so we can compare
    them apples-to-apples without one bottlenecking the other.
    """
    from openai import AsyncOpenAI

    if model_key == "qwen3-vl":
        base_url = settings.vllm_base_url
        model_name = settings.vllm_model
    elif model_key == "qwen2.5-vl":
        base_url = settings.vllm_qwen25_base_url
        model_name = settings.vllm_qwen25_model
    else:
        raise ValueError(f"Unknown vllm model_key: {model_key}")

    image_b64 = _load_image_b64(image_path)

    client = AsyncOpenAI(
        base_url=base_url,
        api_key="not-needed",
        timeout=httpx.Timeout(60.0),
    )

    response = await client.chat.completions.create(
        model=model_name,
        messages=[
            {"role": "system", "content": ANALYSIS_SYSTEM_PROMPT},
            {
                "role": "user",
                "content": [
                    {
                        "type": "image_url",
                        "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"},
                    },
                    {
                        "type": "text",
                        "text": f"Monitoring objective: {objective}",
                    },
                ],
            },
        ],
        temperature=0.1,
        max_tokens=2048,
    )

    content = _extract_completion_text(response, backend_label=f"vLLM ({model_key})")
    parsed = _parse_analysis(content)
    usage = getattr(response, "usage", None)
    if usage is not None:
        parsed["input_tokens"] = getattr(usage, "prompt_tokens", None)
        parsed["output_tokens"] = getattr(usage, "completion_tokens", None)
    return parsed


async def _analyze_with_openrouter(image_path: str, objective: str) -> dict:
    """Analyze image via OpenRouter (OpenAI-compatible API, single key,
    provider-agnostic). Production default as of 2026-08-19 — see config.py
    for the model choice rationale (hybrid routing: this uses
    openrouter_analysis_model, a vision-capable model)."""
    from openai import AsyncOpenAI

    image_b64 = _load_image_b64(image_path)

    client = AsyncOpenAI(
        base_url=settings.openrouter_base_url,
        api_key=settings.openrouter_api_key,
        timeout=httpx.Timeout(60.0),
    )

    response = await client.chat.completions.create(
        model=settings.openrouter_analysis_model,
        messages=[
            {"role": "system", "content": ANALYSIS_SYSTEM_PROMPT},
            {
                "role": "user",
                "content": [
                    {
                        "type": "image_url",
                        "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"},
                    },
                    {
                        "type": "text",
                        "text": f"Monitoring objective: {objective}",
                    },
                ],
            },
        ],
        temperature=0.1,
        max_tokens=2048,
    )

    content = _extract_completion_text(response, backend_label="OpenRouter")
    parsed = _parse_analysis(content)
    usage = getattr(response, "usage", None)
    if usage is not None:
        parsed["input_tokens"] = getattr(usage, "prompt_tokens", None)
        parsed["output_tokens"] = getattr(usage, "completion_tokens", None)
    return parsed


async def _analyze_with_openrouter_retrying(
    image_path: str, objective: str, max_attempts: int = 3
) -> dict:
    """Retry OpenRouter analysis on transient failures. Production evidence
    (67 analyses, 2026-08-18..24): 3 transient failures, none repeating on
    the very next capture of the same scene — so a bounded retry is the
    whole fix. Deliberately does NOT fall back to a different backend: the
    system's claim is that analysis runs on open-weight inference, so
    silently answering from a cloud model would misreport what produced the
    result and corrupt backend-comparison benchmark data. If all attempts
    fail, the caller surfaces an honest error."""
    last_exc: Exception | None = None
    for attempt in range(max_attempts):
        try:
            return await _analyze_with_openrouter(image_path, objective)
        except _OPENROUTER_TRANSIENT_ERRORS as exc:
            last_exc = exc
            if attempt < max_attempts - 1:
                await asyncio.sleep(0.5 * (attempt + 1))

    assert last_exc is not None
    raise last_exc


async def _analyze_with_gemini(image_path: str, objective: str) -> dict:
    """Analyze image using Gemini 3 Flash API."""
    from google import genai
    from google.genai import types

    image_bytes = Path(image_path).read_bytes()

    client = genai.Client(api_key=settings.gemini_api_key)

    response = await client.aio.models.generate_content(
        model=settings.gemini_model,
        contents=[
            types.Content(
                parts=[
                    types.Part.from_bytes(data=image_bytes, mime_type="image/jpeg"),
                    types.Part(text=f"{ANALYSIS_SYSTEM_PROMPT}\n\nMonitoring objective: {objective}"),
                ]
            )
        ],
    )

    if response.text is None:
        # The genai SDK returns None (rather than raising) when there are no
        # candidates or the response was blocked — e.g. a safety filter on
        # the image, or the API silently returning an empty candidate list.
        finish_reason = None
        candidates = getattr(response, "candidates", None) or []
        if candidates:
            finish_reason = getattr(candidates[0], "finish_reason", None)
        raise RuntimeError(
            f"Gemini returned no text content (finish_reason={finish_reason}). "
            "This usually means the response was blocked by a safety filter "
            "or the API returned zero candidates."
        )

    parsed = _parse_analysis(response.text)
    meta = getattr(response, "usage_metadata", None)
    if meta is not None:
        parsed["input_tokens"] = getattr(meta, "prompt_token_count", None)
        parsed["output_tokens"] = getattr(meta, "candidates_token_count", None)
    return parsed


def _extract_completion_text(response, backend_label: str) -> str:
    """Extract message content from an OpenAI-compatible chat completion
    response, raising a clear diagnostic error instead of letting a bare
    ``None`` content silently propagate to ``_parse_analysis`` (which would
    crash with an opaque ``AttributeError: 'NoneType' object has no
    attribute 'strip'``).

    An OpenAI-compatible backend can return HTTP 200 with
    ``choices[0].message.content`` set to ``None`` — without raising —
    when: the upstream provider hit an error/rate-limit and passed it
    through as a "soft" failure (common with OpenRouter's multi-provider
    routing), the response was blocked by a content filter, or a
    reasoning-capable model exhausted ``max_tokens`` on internal reasoning
    before emitting any final-answer content (``finish_reason == "length"``
    with reasoning tokens consumed and no visible output).
    """
    if not response.choices:
        raise RuntimeError(f"{backend_label} returned zero choices — empty response body")

    choice = response.choices[0]
    content = choice.message.content
    finish_reason = getattr(choice, "finish_reason", None)

    if content is None or not content.strip():
        # Some reasoning-capable models expose the (possibly truncated)
        # reasoning trace under a non-standard `reasoning`/`reasoning_content`
        # field even when `content` is empty — surface it for diagnosis.
        reasoning = getattr(choice.message, "reasoning", None) or getattr(
            choice.message, "reasoning_content", None
        )
        reasoning_hint = " (reasoning trace present but no final content — likely truncated by max_tokens)" if reasoning else ""
        raise RuntimeError(
            f"{backend_label} returned empty content (finish_reason={finish_reason})"
            f"{reasoning_hint}. This usually means the upstream provider hit an "
            "error, rate limit, or content filter, or the model exhausted "
            "max_tokens before producing an answer."
        )

    return content


def _parse_analysis(raw_output: str | None) -> dict:
    """Parse LLM output into a structured analysis dict.

    ``raw_output`` should already be a non-empty string by the time it
    reaches here (callers are expected to guard/raise before calling this),
    but this function stays defensive against ``None``/empty input so a
    future backend that forgets to guard degrades gracefully instead of
    crashing with an opaque AttributeError.
    """
    if raw_output is None or not raw_output.strip():
        return {
            "description": "",
            "findings": "The AI backend returned an empty response.",
            "recommendation": "Retry the analysis — the backend returned no content.",
            "flagged": False,
            "flag_reason": "",
        }

    text = raw_output.strip()
    if text.startswith("```"):
        text = text.split("\n", 1)[1]
        text = text.rsplit("```", 1)[0].strip()

    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return {
            "description": text[:500],
            "findings": text,
            "recommendation": "Unable to parse structured analysis — review image manually.",
            "flagged": False,
            "flag_reason": "",
        }

    return {
        "description": data.get("description", ""),
        "findings": data.get("findings", ""),
        "recommendation": data.get("recommendation", ""),
        "flagged": bool(data.get("flagged", False)),
        "flag_reason": data.get("flag_reason", "") or "",
    }


def _load_image_b64(image_path: str) -> str:
    """Load an image file and return its base64 encoding."""
    return base64.b64encode(Path(image_path).read_bytes()).decode("utf-8")
