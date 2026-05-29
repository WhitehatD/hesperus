"""
Thesis IoT Server — Command Queue Tests
Unit tests for the DEEP_DORMANT safety-net chokepoint in app.mqtt.client.

Covered scenarios:
  (a) Command published immediately when board is online.
  (b) Command enqueued when board is deep_dormant.
  (c) Queue flushed in FIFO order when board returns online (via status message).
"""

import json
from unittest.mock import MagicMock, patch

import pytest

import app.mqtt.client as mqtt_module
from app.mqtt.client import send_board_command
from app.config import settings


# ── Helpers ───────────────────────────────────────────────────────────────────

def _reset_board_state(state: str = "online", dormant_until=None, queue=None):
    """Force the module-level state to a known baseline for each test."""
    mqtt_module._board_state = state
    mqtt_module._dormant_until = dormant_until
    mqtt_module._command_queue = list(queue) if queue else []


@pytest.fixture(autouse=True)
def restore_board_state():
    """
    Reset the module-level board state before and after every test in this
    file so that dormant/queue mutations don't leak into other test modules.
    """
    _reset_board_state("online")
    yield
    _reset_board_state("online")


def _make_mock_client():
    mock = MagicMock()
    mock.publish = MagicMock()
    return mock


# ── (a) Immediate publish when online ────────────────────────────────────────

def test_command_published_immediately_when_online():
    """send_board_command publishes straight away when board_state is 'online'."""
    _reset_board_state("online")
    mock_client = _make_mock_client()

    result = send_board_command(mock_client, settings.mqtt_topic_commands, '{"type":"ping"}')

    mock_client.publish.assert_called_once_with(
        settings.mqtt_topic_commands, '{"type":"ping"}'
    )
    assert result == {"queued": False}
    assert mqtt_module._command_queue == []


def test_command_published_when_state_unknown():
    """send_board_command also publishes immediately for 'unknown' state (safe default)."""
    _reset_board_state("unknown")
    mock_client = _make_mock_client()

    result = send_board_command(mock_client, settings.mqtt_topic_commands, '{"type":"capture_now","task_id":1}')

    mock_client.publish.assert_called_once()
    assert result == {"queued": False}


# ── (b) Enqueue when deep_dormant ────────────────────────────────────────────

def test_command_enqueued_when_dormant():
    """send_board_command enqueues the payload when board is deep_dormant."""
    _reset_board_state("deep_dormant", dormant_until="07:30")
    mock_client = _make_mock_client()

    result = send_board_command(mock_client, settings.mqtt_topic_commands, '{"type":"capture_now","task_id":5}')

    mock_client.publish.assert_not_called()
    assert result["queued"] is True
    assert "07:30" in result["reason"]
    assert mqtt_module._command_queue == ['{"type":"capture_now","task_id":5}']


def test_multiple_commands_enqueued_in_order():
    """Multiple commands sent while dormant are all enqueued in FIFO order."""
    _reset_board_state("deep_dormant", dormant_until="08:00")
    mock_client = _make_mock_client()

    payloads = [
        '{"type":"capture_now","task_id":1}',
        '{"type":"ping"}',
        '{"type":"delete_schedule"}',
    ]
    for p in payloads:
        send_board_command(mock_client, settings.mqtt_topic_commands, p)

    mock_client.publish.assert_not_called()
    assert mqtt_module._command_queue == payloads


# ── (c) Queue flushed in FIFO order when board comes online ──────────────────

@pytest.mark.asyncio
async def test_queue_flushed_fifo_on_online_transition():
    """
    When board publishes status=online and queue has pending commands,
    on_message flushes them in FIFO order via mqtt_client.publish.
    """
    payloads = [
        '{"type":"capture_now","task_id":1}',
        '{"type":"ping"}',
        '{"type":"delete_schedule"}',
    ]
    _reset_board_state("deep_dormant", dormant_until="06:00", queue=payloads)

    mock_client = _make_mock_client()

    # on_message uses the module-level mqtt_client for flush — patch it.
    with patch.object(mqtt_module, "mqtt_client", mock_client):
        # Simulate a status=online message arriving from the board.
        raw_msg = json.dumps({"status": "online"}).encode()

        await mqtt_module.on_message(
            mock_client, settings.mqtt_topic_status, raw_msg, 0, {}
        )

    # All three commands must have been published in order.
    assert mock_client.publish.call_count == 3
    call_args = [c[0] for c in mock_client.publish.call_args_list]
    assert call_args[0] == (settings.mqtt_topic_commands, payloads[0])
    assert call_args[1] == (settings.mqtt_topic_commands, payloads[1])
    assert call_args[2] == (settings.mqtt_topic_commands, payloads[2])

    # Queue must be empty and state must be online.
    assert mqtt_module._command_queue == []
    assert mqtt_module._board_state == "online"


@pytest.mark.asyncio
async def test_no_flush_when_already_online():
    """If board is already online, an online status message does not double-publish."""
    _reset_board_state("online", queue=[])
    mock_client = _make_mock_client()

    with patch.object(mqtt_module, "mqtt_client", mock_client):
        raw_msg = json.dumps({"status": "online"}).encode()
        await mqtt_module.on_message(
            mock_client, settings.mqtt_topic_status, raw_msg, 0, {}
        )

    mock_client.publish.assert_not_called()


@pytest.mark.asyncio
async def test_deep_dormant_message_updates_state():
    """status=deep_dormant sets board_state and stores dormant_until."""
    _reset_board_state("online")
    mock_client = _make_mock_client()

    with patch.object(mqtt_module, "mqtt_client", mock_client):
        raw_msg = json.dumps({"status": "deep_dormant", "until": "09:15"}).encode()
        await mqtt_module.on_message(
            mock_client, settings.mqtt_topic_status, raw_msg, 0, {}
        )

    assert mqtt_module._board_state == "deep_dormant"
    assert mqtt_module._dormant_until == "09:15"
    mock_client.publish.assert_not_called()


# ── Board snapshot for dashboard hydration ──────────────────────────────────

@pytest.mark.asyncio
async def test_snapshot_captures_heartbeat_fields():
    """An online heartbeat with lp_mode/firmware populates the hydration snapshot."""
    _reset_board_state("online")
    mock_client = _make_mock_client()

    with patch.object(mqtt_module, "mqtt_client", mock_client):
        raw_msg = json.dumps(
            {
                "status": "online",
                "firmware": "1.0.300.1780000000",
                "uptime_s": 142,
                "lp_mode": "ps_rest",
                "wifi_reconnects": 0,
                "mqtt_reconnects": 2,
            }
        ).encode()
        await mqtt_module.on_message(
            mock_client, settings.mqtt_topic_status, raw_msg, 0, {}
        )

    snap = mqtt_module.get_board_snapshot()
    assert snap["state"] == "online"
    assert snap["lp_mode"] == "ps_rest"
    assert snap["firmware"] == "1.0.300.1780000000"
    assert snap["uptime_s"] == 142
    assert snap["telemetry"]["mqtt_reconnects"] == 2
    assert snap["last_seen"] is not None


@pytest.mark.asyncio
async def test_snapshot_lp_mode_updates_on_transition():
    """lp_mode in the snapshot follows the board: ps_rest then back to normal."""
    _reset_board_state("online")
    mock_client = _make_mock_client()

    with patch.object(mqtt_module, "mqtt_client", mock_client):
        await mqtt_module.on_message(
            mock_client,
            settings.mqtt_topic_status,
            json.dumps({"status": "online", "lp_mode": "ps_rest"}).encode(),
            0,
            {},
        )
        assert mqtt_module.get_board_snapshot()["lp_mode"] == "ps_rest"

        await mqtt_module.on_message(
            mock_client,
            settings.mqtt_topic_status,
            json.dumps({"status": "online", "lp_mode": "normal"}).encode(),
            0,
            {},
        )

    assert mqtt_module.get_board_snapshot()["lp_mode"] == "normal"


def test_board_state_endpoint_returns_snapshot(client):
    """GET /api/board/state returns the aggregated snapshot dict."""
    resp = client.get("/api/board/state")
    assert resp.status_code == 200
    body = resp.json()
    assert "state" in body
    assert "lp_mode" in body
    assert "last_seen" in body
