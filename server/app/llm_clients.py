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

Every LLM client in this codebase must be constructed through one of the two
factories below. Do not call AsyncOpenAI(...) or anthropic.AsyncAnthropic(...)
directly anywhere else — grep for those two symbols before adding a new call
site, and use these instead.
"""

import anthropic
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
