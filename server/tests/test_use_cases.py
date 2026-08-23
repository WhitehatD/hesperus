"""
Thesis IoT Server — Use-Case Simulation Tests
End-to-end scenario tests for thesis evaluation.

UC1  — Single snapshot via /api/capture
UC2  — Burst sequence cleanup (Quick: prefix, >4h TTL)
UC3  — Short schedule cleanup (span < 2h, >48h TTL)
UC4  — Long schedule preserved (span > 2h, never deleted)
UC5  — Active schedule preserved (always immune to cleanup)
UC6  — Modify schedule (re-plans + replaces tasks atomically)
UC7  — Deactivate schedule via _tool_deactivate_schedule
UC8  — cleanup_stale_schedules returns correct deleted count
UC9  — list_schedules tool + world-state snapshot (active schedule + flagged findings)
"""

import os
os.environ["DATABASE_URL"] = "sqlite+aiosqlite://"

import json
from datetime import datetime, timedelta
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy import select

# ── Force module imports before patching (mirrors conftest pattern) ──────────
import app.main           # noqa: F401
import app.api.routes     # noqa: F401
import app.api.scheduler_routes  # noqa: F401
import app.api.firmware_routes   # noqa: F401
import app.api.wifi_routes       # noqa: F401
import app.api.agent_routes      # noqa: F401
import app.mqtt.client           # noqa: F401
import app.db.database           # noqa: F401


# ══════════════════════════════════════════════════════════════════════════════
#  Helpers
# ══════════════════════════════════════════════════════════════════════════════

def _make_engine_and_session():
    """Return a fresh in-memory async engine + session-maker pair."""
    engine = create_async_engine(
        "sqlite+aiosqlite://",
        echo=False,
        connect_args={"check_same_thread": False},
    )
    session_factory = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)
    return engine, session_factory


def _make_mock_mqtt():
    mock = MagicMock()
    mock.connection = AsyncMock()
    mock.client = MagicMock()
    mock.client.disconnect = AsyncMock()
    mock.publish = MagicMock()   # sync publish — firmware uses this path
    return mock


# ══════════════════════════════════════════════════════════════════════════════
#  DB + Table Fixture (async, auto-used per test)
# ══════════════════════════════════════════════════════════════════════════════

@pytest.fixture
async def db_session():
    """
    Yield a live AsyncSession backed by a fresh in-memory SQLite DB.
    All ORM models are registered before tables are created so foreign
    keys and relationships work correctly.
    """
    engine, session_factory = _make_engine_and_session()

    # Register all models so Base.metadata knows about every table
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


# ══════════════════════════════════════════════════════════════════════════════
#  UC1 — Single Snapshot
# ══════════════════════════════════════════════════════════════════════════════

def test_single_snapshot():
    """
    UC1: POST /api/capture should return 200 with a task_id.
    This verifies the board is given a capture_now command.
    """
    from fastapi.testclient import TestClient

    mock_mqtt = _make_mock_mqtt()
    engine, session_factory = _make_engine_and_session()

    with patch.object(app.main, "mqtt_client", mock_mqtt), \
         patch.object(app.api.routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.scheduler_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.firmware_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.wifi_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.agent_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.db.database, "engine", engine), \
         patch.object(app.db.database, "async_session", session_factory):

        from cryptography.fernet import Fernet
        from app.config import settings
        original_key = settings.wifi_config_encryption_key
        settings.wifi_config_encryption_key = Fernet.generate_key().decode()

        from app.main import app as fastapi_app
        with TestClient(fastapi_app) as client:
            resp = client.post("/api/capture")

        settings.wifi_config_encryption_key = original_key

    assert resp.status_code == 200
    data = resp.json()
    assert "task_id" in data
    assert data["task_id"] >= 1
    assert data["status"] == "sent"

    # Verify the MQTT payload
    cmd = json.loads(data["schedule_json"])
    assert cmd["type"] == "capture_now"


# ══════════════════════════════════════════════════════════════════════════════
#  UC2 — Burst Sequence Cleanup
# ══════════════════════════════════════════════════════════════════════════════

async def test_burst_sequence_cleanup(db_session):
    """
    UC2: A 'Quick:' schedule that is inactive and older than 4 h should be deleted
    by cleanup_stale_schedules, even if all its tasks are completed.
    """
    from app.scheduler.service import create_schedule, cleanup_stale_schedules
    from app.scheduler.models import Schedule, ScheduleTask

    # Create the schedule
    schedule = await create_schedule(
        db_session,
        name="Quick: burst 4-shot",
        description="Burst sequence",
        tasks=[
            {"time": "10:00", "action": "CAPTURE_IMAGE", "objective": "shot 1"},
            {"time": "10:01", "action": "CAPTURE_IMAGE", "objective": "shot 2"},
            {"time": "10:02", "action": "CAPTURE_IMAGE", "objective": "shot 3"},
            {"time": "10:03", "action": "CAPTURE_IMAGE", "objective": "shot 4"},
        ],
    )

    # Mark all tasks completed
    for task in schedule.tasks:
        task.completed_at = datetime.now()

    # Make the schedule inactive and old enough to expire
    schedule.is_active = False
    schedule.created_at = datetime.now() - timedelta(hours=5)
    await db_session.commit()

    deleted = await cleanup_stale_schedules(db_session)

    assert deleted == 1

    result = await db_session.execute(
        select(Schedule).where(Schedule.id == schedule.id)
    )
    assert result.scalar_one_or_none() is None, "Expired Quick: schedule must be deleted"


# ══════════════════════════════════════════════════════════════════════════════
#  UC3 — Short Schedule Cleanup
# ══════════════════════════════════════════════════════════════════════════════

async def test_short_schedule_cleanup(db_session):
    """
    UC3: A non-Quick: schedule with span < 2h that has been inactive for > 48 h
    should be cleaned up.
    """
    from app.scheduler.service import create_schedule, cleanup_stale_schedules
    from app.scheduler.models import Schedule

    schedule = await create_schedule(
        db_session,
        name="Short patrol",
        description="Single-hour window",
        tasks=[
            {"time": "09:00", "action": "CAPTURE_IMAGE", "objective": "morning check"},
            {"time": "09:30", "action": "CAPTURE_IMAGE", "objective": "mid-morning check"},
        ],
    )

    # Inactive, no tasks completed (incomplete schedule), old enough to expire
    schedule.is_active = False
    schedule.created_at = datetime.now() - timedelta(days=3)
    await db_session.commit()

    deleted = await cleanup_stale_schedules(db_session)

    assert deleted == 1

    result = await db_session.execute(
        select(Schedule).where(Schedule.id == schedule.id)
    )
    assert result.scalar_one_or_none() is None, "Short stale schedule must be deleted"


# ══════════════════════════════════════════════════════════════════════════════
#  UC4 — Long Schedule Preserved
# ══════════════════════════════════════════════════════════════════════════════

async def test_long_schedule_preserved(db_session):
    """
    UC4: A schedule whose tasks span > 2 h (08:00–20:00) should NEVER be
    auto-deleted regardless of age or inactive status.
    """
    from app.scheduler.service import create_schedule, cleanup_stale_schedules
    from app.scheduler.models import Schedule

    schedule = await create_schedule(
        db_session,
        name="Full-day monitoring",
        description="Spans 12 hours",
        tasks=[
            {"time": "08:00", "action": "CAPTURE_IMAGE", "objective": "morning"},
            {"time": "20:00", "action": "CAPTURE_IMAGE", "objective": "evening"},
        ],
    )

    # Inactive, 30 days old — would normally expire if it were short
    schedule.is_active = False
    schedule.created_at = datetime.now() - timedelta(days=30)
    await db_session.commit()

    deleted = await cleanup_stale_schedules(db_session)

    assert deleted == 0, "Long-span schedule must never be auto-deleted"

    result = await db_session.execute(
        select(Schedule).where(Schedule.id == schedule.id)
    )
    assert result.scalar_one_or_none() is not None, "Long-span schedule must still exist"


# ══════════════════════════════════════════════════════════════════════════════
#  UC5 — Active Schedule Preserved
# ══════════════════════════════════════════════════════════════════════════════

async def test_active_schedule_preserved(db_session):
    """
    UC5: Active schedules are immune to cleanup regardless of age or naming.
    """
    from app.scheduler.service import create_schedule, cleanup_stale_schedules
    from app.scheduler.models import Schedule

    schedule = await create_schedule(
        db_session,
        name="Quick: ongoing monitoring",
        description="Active quick sequence",
        tasks=[
            {"time": "10:00", "action": "CAPTURE_IMAGE", "objective": "check 1"},
        ],
    )

    # Active, but very old
    schedule.is_active = True
    schedule.created_at = datetime.now() - timedelta(days=10)
    await db_session.commit()

    deleted = await cleanup_stale_schedules(db_session)

    assert deleted == 0, "Active schedules must never be deleted"

    result = await db_session.execute(
        select(Schedule).where(Schedule.id == schedule.id)
    )
    assert result.scalar_one_or_none() is not None, "Active schedule must still exist"


# ══════════════════════════════════════════════════════════════════════════════
#  UC6 — Modify Schedule Replaces Tasks Atomically
# ══════════════════════════════════════════════════════════════════════════════

async def test_modify_schedule_updates_tasks(db_session):
    """
    UC6: _tool_modify_schedule must atomically replace the existing tasks
    with the new plan returned by generate_plan.
    Starts with 2 tasks → expects 3 after modification.
    """
    from app.scheduler.service import create_schedule, get_schedule
    from app.api.schemas import PlanResponse, ScheduledTask
    from app.scheduler.models import ScheduleTask

    # --- seed: 2-task schedule ---
    schedule = await create_schedule(
        db_session,
        name="Agent: original plan",
        description="two tasks",
        tasks=[
            {"time": "08:00", "action": "CAPTURE_IMAGE", "objective": "original 1"},
            {"time": "09:00", "action": "CAPTURE_IMAGE", "objective": "original 2"},
        ],
    )
    schedule_id = schedule.id

    # --- build mock plan with 3 new tasks ---
    new_plan = PlanResponse(
        plan_id=99,
        prompt="monitor every hour 10-12",
        tasks=[
            ScheduledTask(time="10:00", action="CAPTURE_IMAGE", id=1, objective="new 1"),
            ScheduledTask(time="11:00", action="CAPTURE_IMAGE", id=2, objective="new 2"),
            ScheduledTask(time="12:00", action="CAPTURE_IMAGE", id=3, objective="new 3"),
        ],
        model_used="mock",
        created_at=datetime.now(),
    )

    mock_mqtt = _make_mock_mqtt()

    with patch("app.api.agent_routes.mqtt_client", mock_mqtt), \
         patch("app.scheduler.notify.mqtt_client", mock_mqtt), \
         patch("app.api.agent_routes.generate_plan", return_value=new_plan) as mock_gen, \
         patch("app.scheduler.notify.async_session", return_value=db_session.__class__) as _, \
         patch("app.api.agent_routes.async_session") as mock_sess_factory, \
         patch("app.scheduler.notify.notify_schedule_update", new_callable=AsyncMock):

        # Wire the patched async_session factory to use our real test session
        from contextlib import asynccontextmanager

        @asynccontextmanager
        async def _fake_ctx():
            yield db_session

        mock_sess_factory.return_value = _fake_ctx()

        from app.api.agent_routes import _tool_modify_schedule
        result = await _tool_modify_schedule({
            "schedule_id": schedule_id,
            "prompt": "monitor every hour 10-12",
        })

    assert result["success"] is True, f"modify failed: {result}"

    # Re-fetch tasks directly from DB
    tasks_result = await db_session.execute(
        select(ScheduleTask).where(ScheduleTask.schedule_id == schedule_id)
    )
    tasks = list(tasks_result.scalars().all())
    assert len(tasks) == 3, f"Expected 3 tasks after modify, got {len(tasks)}"


# ══════════════════════════════════════════════════════════════════════════════
#  UC7 — Deactivate Schedule
# ══════════════════════════════════════════════════════════════════════════════

async def test_deactivate_schedule(db_session):
    """
    UC7: _tool_deactivate_schedule should set is_active=False on the active schedule
    and publish a delete_schedule MQTT command.
    """
    from app.scheduler.service import create_schedule
    from app.scheduler.models import Schedule

    # Create and activate a schedule
    schedule = await create_schedule(
        db_session,
        name="Agent: active patrol",
        description="active schedule",
        tasks=[
            {"time": "14:00", "action": "CAPTURE_IMAGE", "objective": "afternoon check"},
        ],
    )
    schedule.is_active = True
    await db_session.commit()

    mock_mqtt = _make_mock_mqtt()

    with patch("app.api.agent_routes.mqtt_client", mock_mqtt), \
         patch("app.scheduler.notify.mqtt_client", mock_mqtt), \
         patch("app.api.agent_routes.async_session") as mock_sess_factory, \
         patch("app.scheduler.notify.notify_schedule_update", new_callable=AsyncMock):

        from contextlib import asynccontextmanager

        @asynccontextmanager
        async def _fake_ctx():
            yield db_session

        mock_sess_factory.return_value = _fake_ctx()

        from app.api.agent_routes import _tool_deactivate_schedule
        result = await _tool_deactivate_schedule({})

    assert result["success"] is True, f"deactivate failed: {result}"

    # Re-read the schedule from DB and confirm is_active=False
    await db_session.refresh(schedule)
    assert schedule.is_active is False, "Schedule must be inactive after deactivation"

    # MQTT delete_schedule command must have been published
    mock_mqtt.publish.assert_called()
    published_payloads = [call[0][1] for call in mock_mqtt.publish.call_args_list]
    delete_cmds = [p for p in published_payloads if "delete_schedule" in p]
    assert delete_cmds, "Expected a delete_schedule MQTT command"


# ══════════════════════════════════════════════════════════════════════════════
#  UC8 — Cleanup Returns Correct Count
# ══════════════════════════════════════════════════════════════════════════════

async def test_cleanup_returns_count(db_session):
    """
    UC8: cleanup_stale_schedules should return exactly the count of deleted
    schedules. 2 expired Quick: + 1 long-span → 2 deleted, long-span preserved.
    """
    from app.scheduler.service import create_schedule, cleanup_stale_schedules
    from app.scheduler.models import Schedule

    now = datetime.now()

    # --- Quick: schedule 1 — expired ---
    q1 = await create_schedule(
        db_session,
        name="Quick: shot-burst-A",
        description="",
        tasks=[
            {"time": "10:00", "action": "CAPTURE_IMAGE", "objective": ""},
            {"time": "10:01", "action": "CAPTURE_IMAGE", "objective": ""},
        ],
    )
    for t in q1.tasks:
        t.completed_at = now
    q1.is_active = False
    q1.created_at = now - timedelta(hours=5)

    # --- Quick: schedule 2 — expired ---
    q2 = await create_schedule(
        db_session,
        name="Quick: shot-burst-B",
        description="",
        tasks=[
            {"time": "11:00", "action": "CAPTURE_IMAGE", "objective": ""},
            {"time": "11:01", "action": "CAPTURE_IMAGE", "objective": ""},
        ],
    )
    for t in q2.tasks:
        t.completed_at = now
    q2.is_active = False
    q2.created_at = now - timedelta(hours=6)

    # --- Long-span schedule — should be preserved ---
    long_s = await create_schedule(
        db_session,
        name="Full-day surveillance",
        description="spans >2h",
        tasks=[
            {"time": "07:00", "action": "CAPTURE_IMAGE", "objective": "morning"},
            {"time": "19:00", "action": "CAPTURE_IMAGE", "objective": "evening"},
        ],
    )
    long_s.is_active = False
    long_s.created_at = now - timedelta(days=30)

    await db_session.commit()

    deleted = await cleanup_stale_schedules(db_session)

    assert deleted == 2, f"Expected 2 deleted, got {deleted}"

    # Long-span must still be in DB
    result = await db_session.execute(
        select(Schedule).where(Schedule.id == long_s.id)
    )
    assert result.scalar_one_or_none() is not None, "Long-span schedule must not be deleted"

    # Both Quick: must be gone
    for expired_id in (q1.id, q2.id):
        r = await db_session.execute(
            select(Schedule).where(Schedule.id == expired_id)
        )
        assert r.scalar_one_or_none() is None, f"Quick: schedule {expired_id} should be deleted"


# ══════════════════════════════════════════════════════════════════════════════
#  UC9 — list_schedules tool (Problem 1: agent world-state grounding)
# ══════════════════════════════════════════════════════════════════════════════

async def test_tool_list_schedules_returns_active_and_inactive(db_session):
    """
    UC9: _tool_list_schedules must return every schedule (active + inactive)
    with a status marker, giving the agent a defense-in-depth inventory
    beyond the compact world-state snapshot.
    """
    from app.scheduler.service import create_schedule

    active = await create_schedule(
        db_session,
        name="Morning patrol",
        description="",
        tasks=[{"time": "07:00", "action": "CAPTURE_IMAGE", "objective": "check the yard"}],
    )
    active.is_active = True

    inactive = await create_schedule(
        db_session,
        name="Old night watch",
        description="",
        tasks=[{"time": "22:00", "action": "CAPTURE_IMAGE", "objective": "check the gate"}],
    )
    inactive.is_active = False

    await db_session.commit()

    with patch("app.db.database.async_session") as mock_sess_factory:
        from contextlib import asynccontextmanager

        @asynccontextmanager
        async def _fake_ctx():
            yield db_session

        mock_sess_factory.return_value = _fake_ctx()

        from app.api.agent_routes import _tool_list_schedules
        result = await _tool_list_schedules()

    assert result["success"] is True
    assert "2 schedule" in result["summary"]
    assert f"#{active.id}" in result["detail"]
    assert "ACTIVE" in result["detail"]
    assert f"#{inactive.id}" in result["detail"]
    assert "Morning patrol" in result["detail"]
    assert "Old night watch" in result["detail"]


async def test_build_world_state_snapshot_reports_active_schedule_and_flags(db_session):
    """
    Problem 1 + Problem 2 integration: the ambient world-state snapshot must
    surface the active schedule and call out flagged recent findings.
    """
    from app.scheduler.service import create_schedule
    from app.analysis.models import AnalysisResult

    active = await create_schedule(
        db_session,
        name="Doorstep watch",
        description="",
        tasks=[{"time": "09:00", "action": "CAPTURE_IMAGE", "objective": "watch for deliveries"}],
    )
    active.is_active = True

    db_session.add(AnalysisResult(
        task_id=1,
        image_path="a.jpg",
        objective="watch for deliveries",
        analysis="a package appeared on the porch",
        recommendation="check it",
        model_used="test",
        inference_time_ms=5.0,
        flagged=True,
        flag_reason="package left on the doorstep",
    ))
    await db_session.commit()

    with patch("app.db.database.async_session") as mock_sess_factory:
        from contextlib import asynccontextmanager

        @asynccontextmanager
        async def _fake_ctx():
            yield db_session

        mock_sess_factory.return_value = _fake_ctx()

        from app.api.agent_routes import _build_world_state_snapshot
        snapshot = await _build_world_state_snapshot()

    assert f"#{active.id}" in snapshot
    assert "Doorstep watch" in snapshot
    assert "FLAGGED" in snapshot
    assert "package left on the doorstep" in snapshot
