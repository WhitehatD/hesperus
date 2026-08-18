"""
Hesperus Server — Integration Tests
Tests for the capture flow: health → capture → upload → MQTT roundtrip.

Run inside docker: docker compose exec server python -m pytest tests/ -v
"""

import io
import json
from unittest.mock import patch

import pytest


# ══════════════════════════════════════════════════════════════════════════════
#  Health Check
# ══════════════════════════════════════════════════════════════════════════════

def test_health(client):
    """Server should respond with status 'ok'."""
    resp = client.get("/health")
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "ok"


def test_server_time(client):
    """GET /api/time should return valid time components for RTC sync."""
    resp = client.get("/api/time")
    assert resp.status_code == 200
    data = resp.json()

    # All required fields present
    for key in ("hour", "minute", "second", "year", "month", "day", "weekday"):
        assert key in data, f"Missing field: {key}"

    # Valid ranges
    assert 0 <= data["hour"] <= 23
    assert 0 <= data["minute"] <= 59
    assert 0 <= data["second"] <= 59
    assert 0 <= data["year"] <= 99       # 2-digit year
    assert 1 <= data["month"] <= 12
    assert 1 <= data["day"] <= 31
    assert 1 <= data["weekday"] <= 7     # ISO: Monday=1..Sunday=7


# ══════════════════════════════════════════════════════════════════════════════
#  Capture Endpoint (capture_now command)
# ══════════════════════════════════════════════════════════════════════════════

def test_capture_default(client):
    """POST /api/capture should send a capture_now command."""
    resp = client.post("/api/capture")
    assert resp.status_code == 200

    data = resp.json()
    assert data["status"] == "sent"
    assert data["task_id"] >= 1

    command = json.loads(data["schedule_json"])
    assert command["type"] == "capture_now"


def test_capture_custom_objective(client):
    """POST /api/capture with custom objective should return 200."""
    resp = client.post(
        "/api/capture",
        json={"objective": "Check if parking lot is full"},
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "sent"


def test_capture_increments_task_id(client):
    """Each capture call should increment the task_id."""
    resp1 = client.post("/api/capture")
    resp2 = client.post("/api/capture")

    assert resp2.json()["task_id"] > resp1.json()["task_id"]


# ══════════════════════════════════════════════════════════════════════════════
#  Upload Endpoint
# ══════════════════════════════════════════════════════════════════════════════

def test_upload_image(client):
    """POST /api/upload should accept JPEG images and return metadata."""
    fake_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * 100 + b"\xff\xd9"

    resp = client.post(
        "/api/upload",
        params={"task_id": 42},
        files={"file": ("test_capture.jpg", io.BytesIO(fake_jpeg), "image/jpeg")},
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["task_id"] == 42
    assert "task_42_" in data["filename"]
    assert data["filename"].endswith(".jpg")


def test_upload_stores_any_file(client):
    """POST /api/upload accepts any content — the board sends raw pixel data
    which may not match standard image magic bytes."""
    resp = client.post(
        "/api/upload",
        params={"task_id": 1},
        files={"file": ("capture.bin", io.BytesIO(b"\x00" * 50), "application/octet-stream")},
    )
    assert resp.status_code == 200
    assert resp.json()["task_id"] == 1


def test_upload_image_auth_required(client):
    """POST /api/upload should reject unauthenticated uploads when a board
    token is configured — previously unauthenticated regardless of config
    (fixed 2026-08-18 ahead of the infra-core redeploy: shared host, real
    domain, no longer acceptable to leave this endpoint open)."""
    with patch("app.api.routes.settings") as mock_settings:
        mock_settings.firmware_upload_token = "secret-board-token-2026"

        resp = client.post(
            "/api/upload",
            params={"task_id": 1},
            files={"file": ("capture.bin", io.BytesIO(b"\x00" * 50), "application/octet-stream")},
        )
        assert resp.status_code == 403


def test_upload_image_auth_valid(client, tmp_path):
    """POST /api/upload should accept a valid X-Upload-Token."""
    fake_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * 100 + b"\xff\xd9"

    with patch("app.api.routes.settings") as mock_settings:
        mock_settings.firmware_upload_token = "secret-board-token-2026"
        # Must pin upload_dir too — a bare MagicMock attribute stringifies into
        # a literal "MagicMock/settings.upload_dir/..." path and writes real
        # files into the repo working tree.
        mock_settings.upload_dir = str(tmp_path)

        resp = client.post(
            "/api/upload",
            params={"task_id": 42},
            files={"file": ("test_capture.jpg", io.BytesIO(fake_jpeg), "image/jpeg")},
            headers={"X-Upload-Token": "secret-board-token-2026"},
        )
        assert resp.status_code == 200
        assert resp.json()["task_id"] == 42


# ══════════════════════════════════════════════════════════════════════════════
#  MQTT Publish Verification
# ══════════════════════════════════════════════════════════════════════════════

def test_capture_publishes_to_mqtt(client):
    """POST /api/capture should call mqtt_client.publish with capture_now."""
    mock_mqtt = client._mock_mqtt
    mock_mqtt.publish.reset_mock()

    resp = client.post(
        "/api/capture",
        json={"objective": "MQTT publish test"},
    )
    assert resp.status_code == 200
    mock_mqtt.publish.assert_called_once()

    call_args = mock_mqtt.publish.call_args
    topic = call_args[0][0]
    payload = call_args[0][1]

    assert topic == "device/stm32/commands"
    command = json.loads(payload)
    assert command["type"] == "capture_now"
    assert "task_id" in command
    assert command["task_id"] >= 1


def test_low_power_mode_publishes_to_mqtt(client):
    """POST /api/low-power-mode should publish a low_power_mode command."""
    import app.mqtt.client as mqtt_mod

    mqtt_mod._board_state = "online"  # ensure immediate publish, not queued
    mock_mqtt = client._mock_mqtt
    mock_mqtt.publish.reset_mock()

    resp = client.post("/api/low-power-mode", params={"mode": "ps_rest"})
    assert resp.status_code == 200
    assert resp.json()["mode"] == "ps_rest"
    mock_mqtt.publish.assert_called_once()

    payload = mock_mqtt.publish.call_args[0][1]
    command = json.loads(payload)
    assert command["type"] == "low_power_mode"
    assert command["mode"] == "ps_rest"


def test_low_power_mode_rejects_bad_mode(client):
    """POST /api/low-power-mode with an invalid mode returns 400."""
    resp = client.post("/api/low-power-mode", params={"mode": "banana"})
    assert resp.status_code == 400
