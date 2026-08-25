"""
Tests for the agentic analysis layer.
"""

from unittest.mock import MagicMock, patch

import pytest


def test_analyses_empty(client):
    """List analyses returns empty when no analyses exist."""
    resp = client.get("/api/analyses")
    assert resp.status_code == 200
    data = resp.json()
    assert data["analyses"] == []


def test_analysis_404(client):
    """Analysis for unknown task returns 404."""
    resp = client.get("/api/analysis/99999")
    assert resp.status_code == 404


def test_upload_triggers_analysis(client):
    """Upload endpoint fires the background analysis task."""
    fake_image = b"\xff\xd8\xff\xe0" + b"\x00" * 100  # Minimal JPEG header

    with patch("app.api.routes.asyncio") as mock_asyncio:
        mock_asyncio.create_task = lambda coro: coro.close()  # Discard coroutine cleanly
        resp = client.post(
            "/api/upload?task_id=1",
            files={"file": ("test.jpg", fake_image, "image/jpeg")},
        )

    assert resp.status_code == 200
    data = resp.json()
    assert data["task_id"] == 1
    assert data["filename"].endswith(".jpg")


# ── Flagging schema (flagged / flag_reason) ──────────────────────────────


def test_parse_analysis_extracts_flagged_fields():
    """_parse_analysis pulls flagged/flag_reason out of well-formed JSON."""
    from app.analysis.engine import _parse_analysis

    raw = (
        '{"description": "A package on the porch.", '
        '"findings": "A box appeared that was not there before.", '
        '"recommendation": "Check the package.", '
        '"flagged": true, "flag_reason": "package left on the doorstep"}'
    )
    result = _parse_analysis(raw)
    assert result["flagged"] is True
    assert result["flag_reason"] == "package left on the doorstep"


def test_parse_analysis_defaults_flagged_false_when_absent():
    """Older-shaped (or model-truncated) JSON without flagged/flag_reason must
    default safely instead of crashing anything downstream."""
    from app.analysis.engine import _parse_analysis

    raw = '{"description": "Empty driveway.", "findings": "Nothing notable.", "recommendation": "None."}'
    result = _parse_analysis(raw)
    assert result["flagged"] is False
    assert result["flag_reason"] == ""


def test_parse_analysis_malformed_json_defaults_flagged_false():
    """A totally malformed response must still produce safe flagged defaults."""
    from app.analysis.engine import _parse_analysis

    result = _parse_analysis("not json at all")
    assert result["flagged"] is False
    assert result["flag_reason"] == ""


# ── None-response backend gap (production AttributeError fix) ───────────
#
# Regression coverage for the bug where an OpenAI-compatible backend
# (OpenRouter/vLLM) returns HTTP 200 with `choices[0].message.content`
# set to None (no exception raised) — e.g. the upstream provider
# soft-failed, hit a content filter, or a reasoning-capable model
# exhausted max_tokens before emitting final content. Previously this
# propagated straight into `_parse_analysis(None)` and crashed with
# `AttributeError: 'NoneType' object has no attribute 'strip'`, which
# surfaced on the dashboard as "Analysis unavailable — backend error:
# AttributeError...".


def test_parse_analysis_none_input_degrades_gracefully():
    """_parse_analysis must not crash on None — it's the last line of
    defense if a backend forgets to guard before calling it."""
    from app.analysis.engine import _parse_analysis

    result = _parse_analysis(None)
    assert result["flagged"] is False
    assert result["flag_reason"] == ""
    assert "empty response" in result["findings"].lower()


def test_parse_analysis_empty_string_degrades_gracefully():
    """Whitespace-only output is treated the same as None."""
    from app.analysis.engine import _parse_analysis

    result = _parse_analysis("   ")
    assert result["flagged"] is False
    assert "empty response" in result["findings"].lower()


def _make_chat_completion(content, finish_reason="stop", reasoning=None):
    """Build a minimal object shaped like an OpenAI ChatCompletion response,
    with a `choices[0].message.content` / `.finish_reason` for
    _extract_completion_text to inspect."""
    message = MagicMock()
    message.content = content
    message.reasoning = reasoning
    message.reasoning_content = None

    choice = MagicMock()
    choice.message = message
    choice.finish_reason = finish_reason

    response = MagicMock()
    response.choices = [choice]
    return response


def test_extract_completion_text_raises_clear_error_on_none_content():
    """The real root-cause fix: when the backend returns content=None (no
    exception), _extract_completion_text must raise an informative
    RuntimeError instead of letting the None reach _parse_analysis and
    crash with an opaque AttributeError."""
    from app.analysis.engine import _extract_completion_text

    response = _make_chat_completion(content=None, finish_reason="length")

    with pytest.raises(RuntimeError, match="OpenRouter returned empty content"):
        _extract_completion_text(response, backend_label="OpenRouter")


def test_extract_completion_text_error_mentions_reasoning_truncation_hint():
    """When a reasoning trace is present but content is empty, the error
    should hint at max_tokens truncation to make root-causing fast."""
    from app.analysis.engine import _extract_completion_text

    response = _make_chat_completion(
        content=None, finish_reason="length", reasoning="thinking really hard..."
    )

    with pytest.raises(RuntimeError, match="truncated by max_tokens"):
        _extract_completion_text(response, backend_label="OpenRouter")


def test_extract_completion_text_raises_on_zero_choices():
    """An empty choices list must also raise a clear error, not IndexError."""
    from app.analysis.engine import _extract_completion_text

    response = MagicMock()
    response.choices = []

    with pytest.raises(RuntimeError, match="zero choices"):
        _extract_completion_text(response, backend_label="OpenRouter")


def test_extract_completion_text_returns_content_when_present():
    """Sanity check: the happy path still returns the content unchanged."""
    from app.analysis.engine import _extract_completion_text

    response = _make_chat_completion(content='{"description": "ok"}')

    assert _extract_completion_text(response, backend_label="OpenRouter") == '{"description": "ok"}'


def test_analysis_result_model_defaults_flagged_false():
    """AnalysisResult.flagged/flag_reason default to False/"" at INSERT when
    omitted (SQLAlchemy column defaults apply at flush, not construction —
    so this asserts against the persisted row, not the bare Python object)."""
    import asyncio
    from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

    from app.analysis.models import AnalysisResult
    from app.db.database import Base

    async def _run():
        engine = create_async_engine(
            "sqlite+aiosqlite://", connect_args={"check_same_thread": False}
        )
        session_factory = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)

        import app.db.wifi_models  # noqa: F401
        import app.scheduler.models  # noqa: F401
        import app.agent.models  # noqa: F401

        async with engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)

        async with session_factory() as db:
            row = AnalysisResult(
                task_id=1,
                image_path="x.jpg",
                analysis="nothing",
                model_used="test",
                inference_time_ms=1.0,
            )
            db.add(row)
            await db.commit()
            await db.refresh(row)
            return row.flagged, row.flag_reason

    flagged, flag_reason = asyncio.run(_run())
    assert flagged is False
    assert flag_reason == ""


def test_analyses_endpoint_includes_flagged_fields():
    """GET /api/analyses (list_analyses) surfaces flagged/flag_reason per row.

    Uses a self-contained in-memory DB + direct call to the route function
    (bypassing TestClient/FastAPI DI) to avoid mixing event loops with the
    module-scoped `client` fixture.
    """
    import asyncio
    from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

    from app.analysis.models import AnalysisResult
    from app.db.database import Base

    async def _run():
        engine = create_async_engine(
            "sqlite+aiosqlite://", connect_args={"check_same_thread": False}
        )
        session_factory = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)

        import app.db.wifi_models  # noqa: F401
        import app.scheduler.models  # noqa: F401
        import app.agent.models  # noqa: F401

        async with engine.begin() as conn:
            await conn.run_sync(Base.metadata.create_all)

        async with session_factory() as db:
            db.add(AnalysisResult(
                task_id=42,
                image_path="x.jpg",
                objective="watch the door",
                analysis="a box appeared",
                recommendation="check it",
                model_used="test",
                inference_time_ms=10.0,
                flagged=True,
                flag_reason="package left on the doorstep",
            ))
            await db.commit()

            from app.api.routes import list_analyses
            return await list_analyses(db=db)

    result = asyncio.run(_run())
    data = result["analyses"]
    assert len(data) == 1
    assert data[0]["flagged"] is True
    assert data[0]["flag_reason"] == "package left on the doorstep"
