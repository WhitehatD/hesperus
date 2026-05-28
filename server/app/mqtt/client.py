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


def get_board_state() -> str:
    """Return the current tracked board state (for monitoring/debugging)."""
    return _board_state


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
    username=None,
    password=None,
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
