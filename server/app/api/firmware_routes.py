"""
Thesis IoT Server — Firmware OTA Management Routes

Enterprise OTA endpoints for firmware version checking, binary download,
and CI-automated firmware upload. Integrates with the STM32 board's
OTA_CheckForUpdate() HTTP client.

Endpoints:
  GET  /api/firmware/version   → Current firmware metadata (version, size, CRC32)
  GET  /api/firmware/download  → Stream the .bin file to the board
  POST /api/firmware/upload    → Upload new firmware binary (from CI pipeline)
"""

import hashlib
import os
import struct
import json
from datetime import datetime, timezone
from pathlib import Path

from fastapi import APIRouter, File, UploadFile, HTTPException, Header
from fastapi.responses import FileResponse, StreamingResponse

from app.config import settings
from app.api.schemas import FirmwareVersionResponse, FirmwareUploadResponse
from app.mqtt.client import mqtt_client, send_board_command

router = APIRouter()

# ── Firmware storage paths ───────────────────────────────

FIRMWARE_DIR = Path(settings.firmware_dir)
FIRMWARE_BIN = FIRMWARE_DIR / "firmware.bin"
FIRMWARE_META = FIRMWARE_DIR / "firmware.json"
DEPLOY_HISTORY = FIRMWARE_DIR / "deploy_history.json"


def _compute_crc32(data: bytes) -> int:
    """
    Compute CRC32 using the same MPEG-2 polynomial as the firmware.
    Polynomial: 0x04C11DB7 (normal form, MSB-first).
    Initial value: 0xFFFFFFFF, no final XOR.
    """
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def _load_metadata() -> dict | None:
    """Load firmware metadata from disk, or None if no firmware uploaded."""
    if not FIRMWARE_META.exists() or not FIRMWARE_BIN.exists():
        return None
    with open(FIRMWARE_META, "r") as f:
        return json.load(f)


def _save_metadata(meta: dict):
    """Save firmware metadata to disk."""
    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    with open(FIRMWARE_META, "w") as f:
        json.dump(meta, f, indent=2)


def _append_deploy_history(entry: dict):
    """Append a deploy event to the history log (max 50 entries)."""
    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    history = []
    if DEPLOY_HISTORY.exists():
        try:
            with open(DEPLOY_HISTORY, "r") as f:
                history = json.load(f)
        except (json.JSONDecodeError, IOError):
            history = []
    history.append(entry)
    history = history[-50:]  # Keep only last 50 entries
    with open(DEPLOY_HISTORY, "w") as f:
        json.dump(history, f, indent=2)


def _load_deploy_history() -> list:
    """Load deploy history from disk."""
    if not DEPLOY_HISTORY.exists():
        return []
    try:
        with open(DEPLOY_HISTORY, "r") as f:
            return json.load(f)
    except (json.JSONDecodeError, IOError):
        return []


# ══════════════════════════════════════════════════════════════════════════════
#  GET /api/firmware/history
# ══════════════════════════════════════════════════════════════════════════════

@router.get("/firmware/history")
async def get_firmware_history():
    """
    Return the firmware deployment history — a timeline of all CI/CD deploys
    and OTA update triggers. Used by the dashboard to display version history.
    """
    return {"history": _load_deploy_history()}


# ══════════════════════════════════════════════════════════════════════════════
#  GET /api/firmware/version
# ══════════════════════════════════════════════════════════════════════════════

@router.get("/firmware/version", response_model=FirmwareVersionResponse)
async def get_firmware_version():
    """
    Return the current firmware version metadata.
    The STM32 board calls this to check if an update is available.

    Returns 200 with version info, or 404 if no firmware has been uploaded.
    """
    meta = _load_metadata()
    if meta is None:
        raise HTTPException(status_code=404, detail="No firmware binary uploaded")

    return FirmwareVersionResponse(
        version=meta["version"],
        size=meta["size"],
        crc32=meta["crc32"],
        filename=meta.get("filename", "firmware.bin"),
    )


# ══════════════════════════════════════════════════════════════════════════════
#  GET /api/firmware/download
# ══════════════════════════════════════════════════════════════════════════════

@router.get("/firmware/download")
async def download_firmware():
    """
    Stream the firmware binary to the STM32 board.
    The board downloads this during OTA_DownloadAndFlash().

    Returns the raw .bin file with appropriate headers for streaming.
    """
    if not FIRMWARE_BIN.exists():
        raise HTTPException(status_code=404, detail="No firmware binary available")

    from fastapi.responses import FileResponse

    return FileResponse(FIRMWARE_BIN, media_type="application/octet-stream")


# ══════════════════════════════════════════════════════════════════════════════
#  POST /api/firmware/upload
# ══════════════════════════════════════════════════════════════════════════════

@router.post("/firmware/upload", response_model=FirmwareUploadResponse)
async def upload_firmware(
    version: str,
    file: UploadFile = File(...),
    x_firmware_token: str | None = Header(None),
):
    """
    Upload a new firmware binary. Called by the CI/CD pipeline after building.

    Authentication: requires X-Firmware-Token header matching the configured token.
    This prevents unauthorized firmware uploads.

    After upload:
      1. Computes CRC32 for integrity verification
      2. Saves the binary and metadata to disk
      3. Publishes an MQTT notification so the board knows an update is available
    """
    # ── Authentication ────────────────────────────────────
    if settings.firmware_upload_token:
        if x_firmware_token != settings.firmware_upload_token:
            raise HTTPException(status_code=403, detail="Invalid firmware upload token")

    # ── Read and validate binary ──────────────────────────
    content = await file.read()

    if len(content) == 0:
        raise HTTPException(status_code=400, detail="Empty firmware binary")

    if len(content) > 1024 * 1024:  # 1 MB max (one flash bank)
        raise HTTPException(status_code=400, detail="Firmware too large (max 1 MB)")

    # ── Compute CRC32 ────────────────────────────────────
    crc32 = _compute_crc32(content)

    # ── Save to disk ─────────────────────────────────────
    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)

    with open(FIRMWARE_BIN, "wb") as f:
        f.write(content)

    meta = {
        "version": version,
        "size": len(content),
        "crc32": crc32,
        "filename": file.filename or "firmware.bin",
    }
    _save_metadata(meta)

    # ── Notify board via MQTT ────────────────────────────
    update_command = json.dumps({
        "type": "firmware_update",
        "version": version,
        "size": len(content),
    })
    
    try:
        send_board_command(mqtt_client, settings.mqtt_topic_commands, update_command)
    except Exception as exc:
        print(f"[Firmware Upload] WARNING: MQTT publish failed: {exc}")
        # We don't fail the request because the file is safely saved on disk,
        # and the board will eventually fetch it on its 30-min polling cycle.

    # ── Log to deploy history ────────────────────────────
    _append_deploy_history({
        "type": "firmware_upload",
        "version": version,
        "size": len(content),
        "crc32": crc32,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    })

    return FirmwareUploadResponse(
        version=version,
        size=len(content),
        crc32=crc32,
        status="uploaded",
    )


# ══════════════════════════════════════════════════════════════════════════════
#  POST /api/firmware/notify
# ══════════════════════════════════════════════════════════════════════════════

@router.post("/firmware/notify")
async def notify_firmware_update(
    commit_sha: str,
    x_firmware_token: str | None = Header(None),
):
    """
    CI/CD deploy notification — tells the STM32 board to check for updates.

    Called by GitHub Actions after container images are built and pushed.
    Publishes {"type": "firmware_update"} to MQTT so the board initiates
    its OTA version-check flow.

    Authentication: requires X-Firmware-Token header when configured.
    """
    # ── Authentication ────────────────────────────────────
    if settings.firmware_upload_token:
        if x_firmware_token != settings.firmware_upload_token:
            raise HTTPException(status_code=403, detail="Invalid firmware token")

    # ── Publish OTA trigger to MQTT ──────────────────────
    timestamp = datetime.now(timezone.utc).isoformat()

    update_command = json.dumps({
        "type": "firmware_update",
        "source": "ci",
        "commit": commit_sha,
        "timestamp": timestamp,
    })

    try:
        send_board_command(mqtt_client, settings.mqtt_topic_commands, update_command)
    except Exception as exc:
        raise HTTPException(
            status_code=503,
            detail=f"MQTT publish failed: {exc}",
        )

    # ── Log to deploy history ────────────────────────────
    _append_deploy_history({
        "type": "firmware_notify",
        "commit": commit_sha,
        "timestamp": timestamp,
    })

    return {
        "status": "notified",
        "topic": settings.mqtt_topic_commands,
        "commit": commit_sha,
        "timestamp": timestamp,
    }

