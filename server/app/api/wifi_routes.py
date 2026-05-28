"""
Thesis IoT Server — WiFi Configuration Routes
Enterprise-grade remote WiFi credential management for the STM32 board.
"""

import json
from datetime import datetime, timezone

from cryptography.fernet import Fernet
from fastapi import APIRouter, HTTPException
from sqlalchemy import select, update

from app.api.schemas import WifiConfigRequest, WifiConfigResponse
from app.config import settings
from app.db import database as db
from app.db.wifi_models import WifiConfig
from app.mqtt.client import mqtt_client, send_board_command

router = APIRouter()


def _get_fernet() -> Fernet:
    """Return a Fernet cipher using the configured encryption key."""
    key = settings.wifi_config_encryption_key
    if not key:
        raise HTTPException(
            status_code=500,
            detail="WIFI_CONFIG_ENCRYPTION_KEY not configured on server",
        )
    return Fernet(key.encode() if isinstance(key, str) else key)


@router.post("/wifi/config", response_model=WifiConfigResponse)
async def set_wifi_config(request: WifiConfigRequest):
    """
    Set WiFi credentials for the STM32 board.
    Encrypts the password, persists to DB, and publishes via MQTT.
    """
    fernet = _get_fernet()

    # Encrypt password before storage
    password_encrypted = fernet.encrypt(request.password.encode()).decode()

    async with db.async_session() as session:
        async with session.begin():
            # Deactivate all existing configs
            await session.execute(
                update(WifiConfig).values(is_active=False)
            )

            # Insert new active config
            config = WifiConfig(
                ssid=request.ssid,
                password_encrypted=password_encrypted,
                is_active=True,
            )
            session.add(config)

    # Publish MQTT command to the board (plaintext over the internal network)
    command = {
        "type": "set_wifi",
        "ssid": request.ssid,
        "password": request.password,
    }
    send_board_command(
        mqtt_client, settings.mqtt_topic_commands, json.dumps(command)
    )

    return WifiConfigResponse(
        ssid=request.ssid,
        status="sent",
        updated_at=datetime.now(timezone.utc).isoformat(),
    )


@router.get("/wifi/config", response_model=WifiConfigResponse)
async def get_wifi_config():
    """
    Get the current active WiFi SSID (password is NEVER returned).
    """
    async with db.async_session() as session:
        result = await session.execute(
            select(WifiConfig)
            .where(WifiConfig.is_active == True)  # noqa: E712
            .order_by(WifiConfig.updated_at.desc())
            .limit(1)
        )
        config = result.scalar_one_or_none()

    if config is None:
        return WifiConfigResponse(
            ssid="",
            status="no_config",
            updated_at=None,
        )

    return WifiConfigResponse(
        ssid=config.ssid,
        status="active",
        updated_at=config.updated_at.isoformat() if config.updated_at else None,
    )

