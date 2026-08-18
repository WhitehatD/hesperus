"""
Thesis IoT Server — WiFi Configuration Tests
Tests for the WiFi credential management endpoints.
"""

import json
import os

os.environ["DATABASE_URL"] = "sqlite+aiosqlite://"
os.environ["WIFI_CONFIG_ENCRYPTION_KEY"] = "VGVzdEtleTEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNA=="

from cryptography.fernet import Fernet

# Generate a valid Fernet key and set it
_test_key = Fernet.generate_key().decode()
os.environ["WIFI_CONFIG_ENCRYPTION_KEY"] = _test_key

import pytest


# ══════════════════════════════════════════════════════════════════════════════
#  POST /api/wifi/config — Set WiFi Credentials
# ══════════════════════════════════════════════════════════════════════════════

def test_set_wifi_config(client):
    """POST /api/wifi/config with valid SSID/password should return 200."""
    resp = client.post(
        "/api/wifi/config",
        json={"ssid": "TestNetwork", "password": "securepass123"},
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["ssid"] == "TestNetwork"
    assert data["status"] == "sent"
    assert "password" not in data  # Password must NEVER be returned


def test_set_wifi_config_publishes_mqtt(client):
    """POST /api/wifi/config should publish set_wifi command to MQTT."""
    mock_mqtt = client._mock_mqtt
    mock_mqtt.publish.reset_mock()

    resp = client.post(
        "/api/wifi/config",
        json={"ssid": "MQTTTestNet", "password": "mqttpass123"},
    )
    assert resp.status_code == 200
    mock_mqtt.publish.assert_called_once()

    call_args = mock_mqtt.publish.call_args
    topic = call_args[0][0]
    payload = json.loads(call_args[0][1])

    assert topic == "device/stm32/commands"
    assert payload["type"] == "set_wifi"
    assert payload["ssid"] == "MQTTTestNet"
    assert payload["password"] == "mqttpass123"


def test_set_wifi_config_auth_required(client):
    """POST /api/wifi/config should reject unauthenticated requests when a
    board token is configured. Previously completely unauthenticated —
    anyone could remotely push arbitrary WiFi credentials to the board
    (full takeover: redirect it onto attacker-controlled WiFi). Fixed
    2026-08-19 alongside the OTA endpoint auth gap, both found by adversarial
    verification the day after the infra-core redeploy."""
    from unittest.mock import patch

    with patch("app.api.routes.settings") as mock_settings:
        mock_settings.firmware_upload_token = "secret-board-token-2026"

        resp = client.post(
            "/api/wifi/config",
            json={"ssid": "AttackerNet", "password": "hijacked123"},
        )
        assert resp.status_code == 403


def test_set_wifi_config_auth_valid(client):
    """POST /api/wifi/config should accept a valid X-Upload-Token."""
    from unittest.mock import patch

    with patch("app.api.routes.settings") as mock_settings:
        mock_settings.firmware_upload_token = "secret-board-token-2026"

        resp = client.post(
            "/api/wifi/config",
            json={"ssid": "TestNetwork", "password": "securepass123"},
            headers={"X-Upload-Token": "secret-board-token-2026"},
        )
        assert resp.status_code == 200
        assert resp.json()["ssid"] == "TestNetwork"


def test_set_wifi_config_invalid_ssid_empty(client):
    """POST /api/wifi/config with empty SSID should return 422."""
    resp = client.post(
        "/api/wifi/config",
        json={"ssid": "", "password": "validpass123"},
    )
    assert resp.status_code == 422


def test_set_wifi_config_invalid_ssid_too_long(client):
    """POST /api/wifi/config with SSID > 32 chars should return 422."""
    resp = client.post(
        "/api/wifi/config",
        json={"ssid": "A" * 33, "password": "validpass123"},
    )
    assert resp.status_code == 422


def test_set_wifi_config_invalid_password_short(client):
    """POST /api/wifi/config with password < 8 chars should return 422."""
    resp = client.post(
        "/api/wifi/config",
        json={"ssid": "TestNet", "password": "short"},
    )
    assert resp.status_code == 422


def test_set_wifi_config_invalid_password_long(client):
    """POST /api/wifi/config with password > 63 chars should return 422."""
    resp = client.post(
        "/api/wifi/config",
        json={"ssid": "TestNet", "password": "A" * 64},
    )
    assert resp.status_code == 422


# ══════════════════════════════════════════════════════════════════════════════
#  GET /api/wifi/config — Read Current Config
# ══════════════════════════════════════════════════════════════════════════════

def test_get_wifi_config_empty(client):
    """GET /api/wifi/config with no saved config should return no_config."""
    resp = client.get("/api/wifi/config")
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "no_config"
    assert data["ssid"] == ""


def test_get_wifi_config_after_set(client):
    """GET /api/wifi/config after setting should return the active SSID."""
    # Set a config first
    client.post(
        "/api/wifi/config",
        json={"ssid": "ActiveNetwork", "password": "mypassword123"},
    )

    resp = client.get("/api/wifi/config")
    assert resp.status_code == 200
    data = resp.json()
    assert data["ssid"] == "ActiveNetwork"
    assert data["status"] == "active"
    assert "password" not in data  # Password must NEVER be returned


def test_set_wifi_config_overwrites_previous(client):
    """Setting a new config should deactivate the old one."""
    client.post(
        "/api/wifi/config",
        json={"ssid": "FirstNetwork", "password": "password1234"},
    )
    client.post(
        "/api/wifi/config",
        json={"ssid": "SecondNetwork", "password": "password5678"},
    )

    resp = client.get("/api/wifi/config")
    data = resp.json()
    assert data["ssid"] == "SecondNetwork"
    assert data["status"] == "active"
