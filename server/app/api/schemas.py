"""
Thesis IoT Server — API Schemas
Pydantic models for request/response validation.
"""

from datetime import datetime
from pydantic import BaseModel, Field


# ── Planning ─────────────────────────────────────────────

class PlanRequest(BaseModel):
    """User prompt to be decomposed into a task schedule."""
    prompt: str = Field(
        ...,
        description="Natural language monitoring request",
        examples=["Monitor occupancy between 3-5 PM"],
    )
    model: str = Field(
        default="openrouter",
        description="AI model backend: 'openrouter' (production default), 'claude-sonnet', 'claude-haiku', 'qwen3-vl', 'qwen2.5-vl', or 'gemini-3'",
    )


class ScheduledTask(BaseModel):
    """A single task in the generated schedule."""
    time: str = Field(..., description="ISO time for task execution, e.g. '15:00'")
    action: str = Field(default="CAPTURE_IMAGE", description="Action to perform")
    id: int = Field(..., description="Unique task ID")
    objective: str = Field(default="", description="Visual objective for analysis")


class PlanResponse(BaseModel):
    """AI-generated task schedule."""
    plan_id: int
    prompt: str
    tasks: list[ScheduledTask]
    model_used: str
    created_at: datetime


# ── Image Upload ─────────────────────────────────────────

class UploadResponse(BaseModel):
    """Response after image upload."""
    task_id: int
    filename: str
    analysis: str | None = None
    recommendation: str | None = None


# ── Direct Capture ───────────────────────────────────

class CaptureRequest(BaseModel):
    """Request an immediate camera capture (bypasses AI planning)."""
    objective: str = Field(
        default="General visual inspection",
        description="What to look for in the captured image",
    )


class CaptureResponse(BaseModel):
    """Response after sending a capture command."""
    task_id: int
    status: str = "sent"
    schedule_json: str


# ── Results ──────────────────────────────────────────────

class TaskResult(BaseModel):
    """Analysis result for a completed task."""
    task_id: int
    plan_id: int
    image_path: str
    analysis: str
    recommendation: str
    model_used: str
    inference_time_ms: float
    created_at: datetime


# ── Scheduler ────────────────────────────────────────────

class ScheduleTaskCreate(BaseModel):
    """Input for a single task in a schedule."""
    time: str = Field(..., description="Capture time in HH:MM:SS format", examples=["09:30:00"])
    action: str = Field(default="CAPTURE_IMAGE", description="Action to perform")
    objective: str = Field(default="", description="What to look for in the image")


class ScheduleCreate(BaseModel):
    """Input for creating or updating a schedule."""
    name: str = Field(..., description="Schedule name", examples=["Morning Patrol"])
    description: str = Field(default="", description="Optional description")
    tasks: list[ScheduleTaskCreate] = Field(
        ..., min_length=1, description="Ordered list of capture tasks"
    )


class ScheduleTaskOut(BaseModel):
    """Output for a single task."""
    id: int
    time: str
    action: str
    objective: str
    order: int


class ScheduleOut(BaseModel):
    """Output for a full schedule with tasks."""
    id: int
    name: str
    description: str
    is_active: bool
    created_at: str | None = None
    updated_at: str | None = None
    tasks: list[ScheduleTaskOut]


class ScheduleListOut(BaseModel):
    """Wrapper for listing schedules."""
    schedules: list[ScheduleOut]


class ScheduleActivateOut(BaseModel):
    """Response after activating a schedule."""
    schedule_id: int
    status: str = "activated"
    mqtt_payload: str


# ── Firmware OTA ─────────────────────────────────────────

class FirmwareVersionResponse(BaseModel):
    """Firmware version metadata returned to the STM32 board."""
    version: str = Field(..., description="Semantic version string", examples=["0.3"])
    size: int = Field(..., description="Binary size in bytes")
    crc32: int = Field(..., description="CRC32 checksum (MPEG-2 polynomial)")
    filename: str = Field(default="firmware.bin")


class FirmwareUploadResponse(BaseModel):
    """Response after a firmware binary upload from CI."""
    version: str
    size: int
    crc32: int
    status: str = "uploaded"


# ── WiFi Configuration ──────────────────────────────────────

class WifiConfigRequest(BaseModel):
    """Request to update the board's WiFi credentials."""
    ssid: str = Field(
        ...,
        min_length=1,
        max_length=32,
        description="WiFi network name (SSID), max 32 chars per IEEE 802.11",
        examples=["MyNetwork"],
    )
    password: str = Field(
        ...,
        min_length=8,
        max_length=63,
        description="WPA2 passphrase (8-63 characters)",
        examples=["securepassword123"],
    )


class WifiConfigResponse(BaseModel):
    """Response with current WiFi config (password is NEVER returned)."""
    ssid: str
    status: str = Field(
        ..., description="'sent', 'active', or 'no_config'"
    )
    updated_at: str | None = None
