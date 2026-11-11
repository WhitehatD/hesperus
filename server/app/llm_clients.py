"""Single source of truth for constructing LLM API clients.

Root cause this exists to fix (2026-08-25): six separate call sites across
analysis/engine.py, planning/engine.py, api/agent_routes.py, and
api/benchmark_routes.py each constructed their own AsyncOpenAI/AsyncAnthropic
client inline, with timeout policy drifting per site — some had 60s, some
30s, and app/planning/engine.py's two OpenAI clients had NONE at all, which
means the SDK default of 600s applied. Combined with a 3-attempt retry added
the same day, a single hung OpenRouter call could leave a user staring at
"Generating schedule..." for up to 30 minutes with zero feedback — this is
what actually happened in production.

Every LLM client in this codebase must be constructed through one of the
factories below. Do not call AsyncOpenAI(...), anthropic.AsyncAnthropic(...)
or ChatOpenAI(...) directly anywhere else — grep for those symbols before
adding a new call site, and use these instead.
"""

import anthropic
from langchain_openai import ChatOpenAI
from openai import AsyncOpenAI

# A hung upstream call must fail fast enough that a human waiting on a chat
# response notices within one interaction, not one coffee break. 45s is
# comfortably above every real latency observed in this project (analysis
# and planning calls complete in 1-17s per production telemetry) and
# comfortably below "the UI looks broken."
DEFAULT_LLM_TIMEOUT_S = 45.0


def get_openai_client(base_url: str, api_key: str, timeout: float = DEFAULT_LLM_TIMEOUT_S) -> AsyncOpenAI:
    """Construct an OpenAI-compatible client (used for OpenRouter and local
    vLLM/llama.cpp backends) with an enforced request timeout."""
    return AsyncOpenAI(base_url=base_url, api_key=api_key, timeout=timeout)


def get_anthropic_client(api_key: str, timeout: float = DEFAULT_LLM_TIMEOUT_S) -> anthropic.AsyncAnthropic:
    """Construct an Anthropic client with an enforced request timeout."""
    return anthropic.AsyncAnthropic(api_key=api_key, timeout=timeout)


def get_chat_model(
    model: str,
    base_url: str,
    api_key: str,
    timeout: float = DEFAULT_LLM_TIMEOUT_S,
    max_tokens: int | None = None,
    **kwargs,
) -> ChatOpenAI:
    """Construct a LangChain chat model for an OpenAI-compatible endpoint
    (the LangGraph agent and the LangChain chains) with an enforced request
    timeout. Chat Completions only: OpenRouter does not serve the Responses
    API, so langchain-openai must never pick it on its own.

    max_tokens goes in extra_body on purpose: ChatOpenAI(max_tokens=...)
    sends it as "max_completion_tokens" (OpenAI's newer name), while every
    SDK call site here sends "max_tokens", the field OpenRouter documents.
    Sending the same field keeps the two paths identical (asserted in
    tests/test_llm_chains.py) and the output cap guaranteed to apply."""
    if max_tokens is not None:
        kwargs["extra_body"] = {**kwargs.get("extra_body", {}), "max_tokens": max_tokens}
    return ChatOpenAI(
        model=model, base_url=base_url, api_key=api_key, timeout=timeout,
        use_responses_api=False, **kwargs,
    )
