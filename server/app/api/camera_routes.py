"""
Thesis IoT Server — Virtual board (phone camera)

The physical STM32 is gone (2026-11). Its replacement is the dashboard page
/camera opened on a phone. That page:
  - subscribes to the board command topic over MQTT (the dashboard's
    read-only broker login can read device/stm32/#) and reacts to
    capture_now / capture_sequence / ping
  - uploads each photo as a JPEG to POST /api/upload, authenticated with the
    owner login cookie instead of the board token (verify_board_token)
  - reports its status through POST /api/camera/status below

Why status goes through the server: the browser's MQTT login cannot publish
(mosquitto/acl, on purpose: its password is in the public JS bundle). The
server connects as hesperus-board, so it republishes the status on the
board's own status topic. Its own on_message then sees it exactly like a
real board heartbeat: board state, the world-state snapshot, the command
queue flush and the fleet page all work unchanged.

No schedules in phone mode: the board ran schedules itself, the page only
reacts to commands while it is open.
"""

import json

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from app.config import settings
from app.mqtt.client import mqtt_client

router = APIRouter(prefix="/camera", tags=["Virtual board"])

# Same status values the firmware publishes. Anything else is refused so the
# page can't inject arbitrary fields into the board's status stream.
ALLOWED_STATUSES = {"online", "ping_received", "capturing", "uploading", "uploaded", "error"}

PHONE_FIRMWARE = "phone-camera"


class CameraStatus(BaseModel):
    status: str
    task_id: int | None = None
    detail: str = Field(default="", max_length=200)


@router.post("/status")
async def camera_status(body: CameraStatus):
    if body.status not in ALLOWED_STATUSES:
        raise HTTPException(status_code=422, detail=f"Unknown status '{body.status}'")
    payload: dict = {"status": body.status, "firmware": PHONE_FIRMWARE}
    if body.task_id is not None:
        payload["task_id"] = body.task_id
    if body.detail:
        payload["detail"] = body.detail
    mqtt_client.publish(settings.mqtt_topic_status, json.dumps(payload))
    return {"ok": True}
