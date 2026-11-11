"""
Thesis IoT Server — LangChain chains vs their SDK twins

Schedule planning and image analysis each have two implementations of the
OpenRouter call, picked by settings.llm_framework: the hand-written SDK call
(_plan_with_openrouter_retrying, _analyze_with_openrouter_retrying) and a
LangChain chain (build_planning_chain, build_analysis_chain). Each test runs
both against the same fake OpenRouter and asserts they send the same request
and return the same result, including the 3-attempt retry and the
empty-content guard.

Faked on httpx2's transport for the same reason as test_agent_engines.py:
openai 3.x and langchain-openai both send through httpx2.
"""

import json
from unittest.mock import patch

import httpx2
import pytest

from app.analysis import engine as analysis_engine
from app.config import settings
from app.planning import engine as planning_engine

FRAMEWORKS = ["sdk", "langchain"]

SCHEDULE_JSON = json.dumps({"tasks": [
    {"time": "22:00", "action": "CAPTURE_IMAGE", "id": 1, "objective": "check the door"},
    {"time": "22:10", "action": "CAPTURE_IMAGE", "id": 2, "objective": "check for changes"},
]})
ANALYSIS_JSON = json.dumps({
    "description": "A hallway with a closed door.",
    "findings": "The door is closed.",
    "recommendation": "No action needed.",
    "flagged": False,
    "flag_reason": "",
})


@pytest.fixture(autouse=True)
def _openrouter_key(monkeypatch):
    monkeypatch.setattr(settings, "openrouter_api_key", "test-key")
    # The SDK path sleeps 0.5 s / 1 s between attempts; the chain's
    # with_retry backs off on its own. Neither matters for these tests.
    monkeypatch.setattr(planning_engine.asyncio, "sleep", _no_sleep)
    monkeypatch.setattr(analysis_engine.asyncio, "sleep", _no_sleep)


async def _no_sleep(*_args, **_kwargs):
    return None


def _completion(content: str | None, finish_reason: str = "stop") -> dict:
    return {
        "id": "chatcmpl-test", "object": "chat.completion", "created": 0, "model": "test-model",
        "choices": [{"index": 0, "message": {"role": "assistant", "content": content},
                     "finish_reason": finish_reason}],
        "usage": {"prompt_tokens": 120, "completion_tokens": 30, "total_tokens": 150},
    }


class FakeOpenRouter:
    """Serves the scripted responses in order (repeating the last one) and
    records every request body. A response can be an int: an HTTP error
    status with no body."""

    def __init__(self, *responses):
        self.responses = list(responses)
        self.requests: list[dict] = []

    async def handle(self, transport, request):
        assert str(request.url).endswith("/chat/completions"), request.url
        self.requests.append(json.loads(await request.aread()))
        r = self.responses.pop(0) if len(self.responses) > 1 else self.responses[0]
        if isinstance(r, int):
            return httpx2.Response(r, json={"error": {"message": "upstream"}}, request=request)
        return httpx2.Response(200, json=r, request=request)

    def __enter__(self):
        async def _handle(transport, request):
            return await self.handle(transport, request)
        self._patch = patch.object(httpx2.AsyncHTTPTransport, "handle_async_request", _handle)
        self._patch.start()
        return self

    def __exit__(self, *exc):
        self._patch.stop()


def _request_shape(body: dict) -> dict:
    """The parts of a request the model actually sees, so the two
    implementations can be compared regardless of extra defaults a library
    adds (e.g. stream=false)."""
    return {k: body.get(k) for k in ("model", "messages", "temperature", "max_tokens")}


# ── Planning ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("framework", FRAMEWORKS)
async def test_generate_plan_returns_the_schedule(framework, monkeypatch):
    monkeypatch.setattr(settings, "llm_framework", framework)
    with FakeOpenRouter(_completion(SCHEDULE_JSON)) as fake:
        plan = await planning_engine.generate_plan("watch the door tonight", "openrouter")

    assert [(t.id, t.time, t.objective) for t in plan.tasks] == [
        (1, "22:00", "check the door"), (2, "22:10", "check for changes"),
    ]
    assert len(fake.requests) == 1


async def test_planning_chain_sends_the_same_request_as_the_sdk(monkeypatch):
    shapes = {}
    for framework in FRAMEWORKS:
        monkeypatch.setattr(settings, "llm_framework", framework)
        with FakeOpenRouter(_completion(SCHEDULE_JSON)) as fake:
            await planning_engine.generate_plan("watch the door tonight", "openrouter")
        shapes[framework] = _request_shape(fake.requests[0])

    assert shapes["sdk"] == shapes["langchain"]
    assert shapes["sdk"]["messages"][0] == {"role": "system", "content": planning_engine.PLANNING_SYSTEM_PROMPT}


@pytest.mark.parametrize("framework", FRAMEWORKS)
async def test_planning_retries_a_malformed_answer_then_succeeds(framework, monkeypatch):
    monkeypatch.setattr(settings, "llm_framework", framework)
    with FakeOpenRouter(_completion("not json at all"), _completion(SCHEDULE_JSON)) as fake:
        plan = await planning_engine.generate_plan("watch the door", "openrouter")

    assert len(plan.tasks) == 2
    assert len(fake.requests) == 2


@pytest.mark.parametrize("framework", FRAMEWORKS)
async def test_planning_gives_up_after_three_attempts(framework, monkeypatch):
    monkeypatch.setattr(settings, "llm_framework", framework)
    with FakeOpenRouter(_completion("still not json")) as fake, pytest.raises(ValueError):
        await planning_engine.generate_plan("watch the door", "openrouter")

    assert len(fake.requests) == 3


# ── Analysis ────────────────────────────────────────────────────────────────

@pytest.fixture
def image_path(tmp_path):
    p = tmp_path / "capture.jpg"
    p.write_bytes(b"\xff\xd8\xff\xe0fake-jpeg-bytes\xff\xd9")
    return str(p)


@pytest.mark.parametrize("framework", FRAMEWORKS)
async def test_analyze_image_returns_parsed_fields_and_tokens(framework, monkeypatch, image_path):
    monkeypatch.setattr(settings, "llm_framework", framework)
    with FakeOpenRouter(_completion(ANALYSIS_JSON)):
        result = await analysis_engine.analyze_image(image_path, "check the door", "openrouter")

    assert result["findings"] == "The door is closed."
    assert result["flagged"] is False
    assert result["input_tokens"] == 120
    assert result["output_tokens"] == 30
    assert result["model_used"] == "openrouter"


async def test_analysis_chain_sends_the_same_request_as_the_sdk(monkeypatch, image_path):
    shapes, results = {}, {}
    for framework in FRAMEWORKS:
        monkeypatch.setattr(settings, "llm_framework", framework)
        with FakeOpenRouter(_completion(ANALYSIS_JSON)) as fake:
            r = await analysis_engine.analyze_image(image_path, "check the door", "openrouter")
        shapes[framework] = _request_shape(fake.requests[0])
        r.pop("inference_time_ms")
        results[framework] = r

    assert shapes["sdk"] == shapes["langchain"]
    assert results["sdk"] == results["langchain"]
    user_content = shapes["langchain"]["messages"][1]["content"]
    assert user_content[0]["image_url"]["url"].startswith("data:image/jpeg;base64,")
    assert user_content[1] == {"type": "text", "text": "Monitoring objective: check the door"}


@pytest.mark.parametrize("framework", FRAMEWORKS)
async def test_analysis_retries_empty_content_and_server_errors(framework, monkeypatch, image_path):
    monkeypatch.setattr(settings, "llm_framework", framework)
    with FakeOpenRouter(_completion(None, finish_reason="length"), _completion(ANALYSIS_JSON)) as fake:
        result = await analysis_engine.analyze_image(image_path, "check the door", "openrouter")

    assert result["findings"] == "The door is closed."
    assert len(fake.requests) == 2


@pytest.mark.parametrize("framework", FRAMEWORKS)
async def test_analysis_gives_up_on_repeated_empty_content(framework, monkeypatch, image_path):
    monkeypatch.setattr(settings, "llm_framework", framework)
    with FakeOpenRouter(_completion(None, finish_reason="length")) as fake, \
            pytest.raises(RuntimeError, match="empty content"):
        await analysis_engine.analyze_image(image_path, "check the door", "openrouter")

    assert len(fake.requests) == 3
