"""
Thesis IoT Server — MQTT Client
Async MQTT integration using fastapi-mqtt.

Board command chokepoint
------------------------
All firmware commands must go through ``send_board_command`` instead of calling
``mqtt_client.publish(settings.mqtt_topic_commands, ...)`` directly.  This lets
the server queue commands when the board is in DEEP_DORMANT mode (~2 µA, WiFi
off) and replay them in FIFO order as soon as the board comes back online.

Limitation: the queue is in-memory only.  A server restart while the board is
dormant will lose queued commands.  Durable persistence (SQLite-backed queue)
is left as future work.
"""

import json
from typing import Optional

from fastapi_mqtt import FastMQTT, MQTTConfig

from app.config import settings


# ── Board liveness state ──────────────────────────────────────────────────────
# Possible values: "online" | "deep_dormant" | "unknown"
_board_state: str = "unknown"
_dormant_until: Optional[str] = None          # e.g. "07:30" from firmware msg
_command_queue: list[str] = []                # payloads pending flush, FIFO

# ── Last-known board snapshot ─────────────────────────────────────────────────
# Aggregated from the MQTT status stream so the dashboard can hydrate its full
# state on page load (REST) instead of waiting for the next sparse heartbeat
# (heartbeats are rare in PS-REST, where the board sleeps ~97% of the time).
_board_snapshot: dict = {
    "state": "unknown",        # online | deep_dormant | unknown
    "lp_mode": None,           # "ps_rest" | "normal" | None
    "sleep_mode": None,        # bool — deep-sleep armed (reported by heartbeat)
    "firmware": None,
    "uptime_s": None,
    "dormant_until": None,
    "last_seen": None,         # ISO-8601 UTC of last status message
    "telemetry": {},           # wifi/mqtt reconnects, capture/ota failures
    "upload_progress": None,   # {task_id, bytes_sent, bytes_total, progress} | None
}


def get_board_state() -> str:
    """Return the current tracked board state (for monitoring/debugging)."""
    return _board_state


def get_board_snapshot() -> dict:
    """Return the last-known aggregated board snapshot for dashboard hydration."""
    return dict(_board_snapshot)


def get_upload_progress(task_id: int) -> dict | None:
    """Return the latest known chunk-upload progress for *task_id*, or None
    if no progress has been reported for it (older firmware without the
    per-chunk "uploading" status publish, or the upload hasn't started yet).
    Used by the chat SSE stream (agent_routes.py) to show real upload
    percentage inline in the conversation instead of only elapsed seconds."""
    prog = _board_snapshot.get("upload_progress")
    if prog is not None and prog.get("task_id") == task_id:
        return prog
    return None


def note_power_command(lp_mode: Optional[str] = None, sleep_mode: Optional[bool] = None) -> None:
    """Optimistically reflect a just-sent power-mode command in the snapshot.

    The board confirms its real state in the next heartbeat (which carries both
    lp_mode and sleep_mode), but heartbeats are sparse in PS-REST/Sleep — so the
    dashboard's 5 s snapshot poll would otherwise reconcile the toggle back to
    the stale mode for several seconds. Updating the snapshot here gives instant,
    correct feedback; the heartbeat remains the source of truth and overwrites it.
    The board enforces exclusivity, so mirror it here too.
    """
    if lp_mode is not None:
        _board_snapshot["lp_mode"] = lp_mode
        _board_snapshot["sleep_mode"] = False  # any lp_mode cmd is exclusive with sleep
    if sleep_mode is not None:
        _board_snapshot["sleep_mode"] = sleep_mode
        if sleep_mode:
            _board_snapshot["lp_mode"] = "normal"


# ── Chokepoint ────────────────────────────────────────────────────────────────

def send_board_command(client, topic: str, payload: str) -> dict:
    """Publish *payload* to the board command topic through the chokepoint.

    Parameters
    ----------
    client:
        The ``mqtt_client`` instance to use for publishing.  Callers pass
        their module-level reference so that test mocks (which patch the
        per-module name) are honoured correctly.
    topic:
        The MQTT topic (always ``settings.mqtt_topic_commands`` in practice).
    payload:
        JSON string to send.

    Returns
    -------
    dict
        ``{"queued": False}`` when published immediately, or
        ``{"queued": True, "reason": "board dormant until HH:MM"}`` when the
        board is in DEEP_DORMANT and the command was enqueued.
    """
    global _board_state, _dormant_until, _command_queue

    if _board_state == "deep_dormant":
        _command_queue.append(payload)
        reason = f"board dormant until {_dormant_until}" if _dormant_until else "board dormant"
        print(f"[QUEUE] Command queued ({len(_command_queue)} pending): {payload[:60]}")
        return {"queued": True, "reason": reason}

    client.publish(topic, payload)
    return {"queued": False}


def _flush_queue() -> None:
    """Publish all queued commands in FIFO order using the module-level client.

    Called internally when the board transitions to 'online'.  Uses the
    module-level ``mqtt_client`` directly because flush happens inside
    ``on_message``, which is already bound to that instance.
    """
    global _command_queue

    if not _command_queue:
        return

    flushing = list(_command_queue)
    _command_queue.clear()
    print(f"[QUEUE] Flushing {len(flushing)} queued command(s) to board.")
    for payload in flushing:
        mqtt_client.publish(settings.mqtt_topic_commands, payload)


# ── MQTT plumbing ─────────────────────────────────────────────────────────────

mqtt_config = MQTTConfig(
    host=settings.mqtt_broker_host,
    port=settings.mqtt_broker_port,
    keepalive=60,
    username=settings.mqtt_username or None,
    password=settings.mqtt_password or None,
)

mqtt_client = FastMQTT(config=mqtt_config)


@mqtt_client.on_connect()
def on_connect(client, flags, rc, properties):
    """Called when MQTT connection is established."""
    mqtt_client.client.subscribe(settings.mqtt_topic_status)
    print(f"[OK] Subscribed to: {settings.mqtt_topic_status}")


@mqtt_client.on_message()
async def on_message(client, topic, payload, qos, properties):
    """Handle incoming MQTT messages from the STM32 board."""
    global _board_state, _dormant_until

    message = payload.decode()

    try:
        data = json.loads(message)
    except (json.JSONDecodeError, ValueError):
        return

    status = data.get("status")

    # ── Board liveness tracking ───────────────────────────────────────────────
    if status == "online":
        if _board_state != "online":
            print(f"[BOARD] Transitioned to online (was: {_board_state})")
            _board_state = "online"
            _dormant_until = None
            _flush_queue()
        else:
            _board_state = "online"
            _dormant_until = None

    elif status == "deep_dormant":
        _dormant_until = data.get("until")
        _board_state = "deep_dormant"
        print(f"[BOARD] Board entered DEEP_DORMANT (until={_dormant_until})")

    elif status is not None:
        # Any other known-online status (heartbeat, cycle_complete, etc.)
        # transitions the board to "online" and flushes the queue.
        if _board_state == "deep_dormant":
            print(f"[BOARD] Board woke (status={status!r}); was deep_dormant. Flushing queue.")
            _board_state = "online"
            _dormant_until = None
            _flush_queue()
        elif _board_state != "online":
            _board_state = "online"
            _dormant_until = None

    # ── Aggregate last-known snapshot for dashboard hydration ────────────────
    # Mutate in place (dict) so a page refresh can GET the full current state.
    if status is not None:
        from datetime import datetime, timezone

        _board_snapshot["state"] = _board_state
        _board_snapshot["dormant_until"] = _dormant_until
        _board_snapshot["last_seen"] = datetime.now(timezone.utc).isoformat()
        if data.get("lp_mode") is not None:
            _board_snapshot["lp_mode"] = data["lp_mode"]
        if "sleep_mode" in data:
            # heartbeat sends 0/1; deep_dormant implies sleep is armed
            _board_snapshot["sleep_mode"] = bool(data["sleep_mode"])
        elif status == "deep_dormant":
            _board_snapshot["sleep_mode"] = True
        if data.get("firmware") is not None:
            _board_snapshot["firmware"] = data["firmware"]
        if data.get("uptime_s") is not None:
            _board_snapshot["uptime_s"] = data["uptime_s"]
        _tele = {
            k: data[k]
            for k in ("wifi_reconnects", "mqtt_reconnects", "capture_failures", "ota_failures")
            if k in data
        }
        if _tele:
            _board_snapshot["telemetry"] = _tele

    # ── Live chunk-upload progress (per task, for chat-inline display) ───────
    # Consumed by app.api.agent_routes' capture SSE stream via
    # get_upload_progress() so the chat conversation shows real bytes/percent
    # instead of only elapsed seconds. Cleared once the task leaves the
    # uploading phase so a stale percentage from a finished task can't bleed
    # into the next capture's wait loop.
    if status == "uploading" and data.get("task_id") is not None:
        _board_snapshot["upload_progress"] = {
            "task_id": data.get("task_id"),
            "bytes_sent": data.get("bytes_sent"),
            "bytes_total": data.get("bytes_total"),
            "progress": data.get("progress"),
        }
    elif status in ("uploaded", "error", "cycle_complete"):
        _board_snapshot["upload_progress"] = None

    # ── Energy phase-timer telemetry (RQ3 measured duty cycle) ───────────────
    if status == "energy":
        try:
            from app.db.database import async_session
            from app.analysis.models import EnergyTelemetry
            from sqlalchemy.exc import SQLAlchemyError

            try:
                async with async_session() as db:
                    db.add(EnergyTelemetry(
                        window_ms=int(data.get("window_ms", 0)),
                        ps_rest_ms=int(data.get("ps_rest_ms", 0)),
                        capture_ms=int(data.get("capture_ms", 0)),
                    ))
                    await db.commit()
            except SQLAlchemyError as e:
                print(f"[ENERGY] persist failed: {e}")
        except (ValueError, TypeError) as e:
            print(f"[ENERGY] bad telemetry payload: {e}")

    # ── Auto-deactivate schedule when board finishes all tasks ───────────────
    if status == "cycle_complete":
        try:
            from app.db.database import async_session
            from app.scheduler.service import list_schedules
            from sqlalchemy import update
            from app.scheduler.models import Schedule

            async with async_session() as db:
                active = [s for s in await list_schedules(db) if s.is_active]
                if active:
                    await db.execute(
                        update(Schedule).where(Schedule.is_active == True).values(is_active=False)
                    )
                    await db.commit()
                    print(f"[OK] Auto-deactivated schedule: {active[0].name}")
                    from app.scheduler.notify import notify_schedule_update
                    await notify_schedule_update()
        except Exception as e:
            print(f"[WARN] Failed to deactivate schedule: {e}")


@mqtt_client.on_disconnect()
def on_disconnect(client, packet, exc=None):
    """Called when MQTT connection is lost."""
    print("[WARN] MQTT disconnected -- will auto-reconnect")
