"""
Thesis IoT Server — Scheduler API Routes
CRUD + activation endpoints for monitoring schedules.
"""

import json

from fastapi import APIRouter, Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import settings
from app.db.database import get_db
from app.mqtt.client import mqtt_client, send_board_command
from app.scheduler import service
from app.scheduler.notify import notify_schedule_update
from app.api.schemas import (
    ScheduleCreate,
    ScheduleOut,
    ScheduleListOut,
    ScheduleActivateOut,
)

router = APIRouter(prefix="/schedules", tags=["Scheduler"])


@router.post("", response_model=ScheduleOut, status_code=201)
async def create_schedule(body: ScheduleCreate, db: AsyncSession = Depends(get_db)):
    """Create a new monitoring schedule with tasks."""
    schedule = await service.create_schedule(
        db,
        name=body.name,
        description=body.description,
        tasks=[t.model_dump() for t in body.tasks],
    )
    return _schedule_to_dict(schedule)


@router.get("", response_model=ScheduleListOut)
async def list_schedules(db: AsyncSession = Depends(get_db)):
    """List all saved schedules."""
    schedules = await service.list_schedules(db)
    return {"schedules": [_schedule_to_dict(s) for s in schedules]}


@router.get("/{schedule_id}", response_model=ScheduleOut)
async def get_schedule(schedule_id: int, db: AsyncSession = Depends(get_db)):
    """Get a schedule by ID with its full task list."""
    schedule = await service.get_schedule(db, schedule_id)
    return _schedule_to_dict(schedule)


@router.put("/{schedule_id}", response_model=ScheduleOut)
async def update_schedule(
    schedule_id: int, body: ScheduleCreate, db: AsyncSession = Depends(get_db)
):
    """Update a schedule's name, description, and tasks.

    If the schedule is currently active, re-publishes the updated task list
    to the board via MQTT so it takes effect immediately.
    """
    schedule = await service.update_schedule(
        db,
        schedule_id=schedule_id,
        name=body.name,
        description=body.description,
        tasks=[t.model_dump() for t in body.tasks],
    )

    # Re-publish to board if active
    if schedule.is_active:
        mqtt_payload = await service.activate_schedule(db, schedule_id)
        send_board_command(mqtt_client, settings.mqtt_topic_commands, json.dumps(mqtt_payload))
        # activate_schedule commits and expires the session — re-fetch with eager-loaded tasks
        schedule = await service.get_schedule(db, schedule_id)

    await notify_schedule_update()
    return _schedule_to_dict(schedule)


@router.post("/{schedule_id}/activate", response_model=ScheduleActivateOut)
async def activate_schedule(schedule_id: int, db: AsyncSession = Depends(get_db)):
    """
    Activate a schedule — deactivates all others and publishes
    the task list to the STM32 board via MQTT.
    """
    mqtt_payload = await service.activate_schedule(db, schedule_id)
    payload_json = json.dumps(mqtt_payload)

    send_board_command(mqtt_client, settings.mqtt_topic_commands, payload_json)

    await notify_schedule_update()

    return ScheduleActivateOut(
        schedule_id=schedule_id,
        status="activated",
        mqtt_payload=payload_json,
    )


@router.post("/{schedule_id}/deactivate", status_code=200)
async def deactivate_schedule(schedule_id: int, db: AsyncSession = Depends(get_db)):
    """Deactivate a schedule and tell the board to clear it."""
    await service.deactivate_schedule(db, schedule_id)
    payload = json.dumps({"type": "delete_schedule"})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, payload)
    await notify_schedule_update()
    return {"status": "deactivated", "schedule_id": schedule_id}


@router.post("/sleep-mode", status_code=200)
async def set_sleep_mode(enabled: bool = True):
    """Toggle sleep mode on the STM32 board via MQTT."""
    payload = json.dumps({"type": "sleep_mode", "enabled": enabled})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, payload)
    return {"status": "ok", "sleep_enabled": enabled}


@router.delete("/{schedule_id}", status_code=204)
async def delete_schedule(schedule_id: int, db: AsyncSession = Depends(get_db)):
    """Delete a schedule and notify the board to clear it."""
    await service.delete_schedule(db, schedule_id)
    payload = json.dumps({"type": "delete_schedule", "schedule_id": schedule_id})
    send_board_command(mqtt_client, settings.mqtt_topic_commands, payload)
    await notify_schedule_update()


# ── Helpers ──────────────────────────────────────────────

def _schedule_to_dict(schedule) -> dict:
    """Convert ORM model to dict matching ScheduleOut schema."""
    return {
        "id": schedule.id,
        "name": schedule.name,
        "description": schedule.description,
        "is_active": schedule.is_active,
        "created_at": schedule.created_at.isoformat() if schedule.created_at else None,
        "updated_at": schedule.updated_at.isoformat() if schedule.updated_at else None,
        "tasks": [
            {
                "id": t.id,
                "time": t.time,
                "action": t.action,
                "objective": t.objective,
                "order": t.order,
                "completed_at": t.completed_at.isoformat() if t.completed_at else None,
            }
            for t in schedule.tasks
        ],
    }
