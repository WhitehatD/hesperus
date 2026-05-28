"""
Thesis IoT Server — Analysis ORM Models
Stores multimodal LLM analysis results for captured images.
"""

from datetime import datetime

from sqlalchemy import Boolean, Column, DateTime, Float, ForeignKey, Integer, String, Text, func
from sqlalchemy.orm import Mapped, mapped_column

from app.db.database import Base


class AnalysisResult(Base):
    """Result of multimodal LLM analysis on a captured image."""

    __tablename__ = "analysis_results"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    task_id: Mapped[int] = mapped_column(Integer, nullable=False, index=True)
    image_path: Mapped[str] = mapped_column(String(512), nullable=False)
    objective: Mapped[str] = mapped_column(Text, default="")
    analysis: Mapped[str] = mapped_column(Text, nullable=False)
    recommendation: Mapped[str] = mapped_column(Text, default="")
    model_used: Mapped[str] = mapped_column(String(64), nullable=False)
    inference_time_ms: Mapped[float] = mapped_column(Float, nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime, server_default=func.now()
    )


class CaptureLatency(Base):
    """Per-capture end-to-end latency record keyed by task_id.

    Timestamps are wall-clock epoch seconds (float, ms precision).
    Rows are upserted by task_id via benchmark.timing.record().
    """

    __tablename__ = "capture_latency"

    id = Column(Integer, primary_key=True, autoincrement=True)
    task_id = Column(Integer, index=True, nullable=False)
    run_id = Column(String(64), index=True)
    capture_type = Column(String(16))  # "single" or "sequence"
    model_key = Column(String(64))
    # Wall-clock timestamps (epoch seconds, float for ms precision)
    t_request = Column(Float)
    t_plan_start = Column(Float)
    t_plan_end = Column(Float)
    t_mqtt_sent = Column(Float)
    t_upload_received = Column(Float)
    t_jpeg_converted = Column(Float)
    t_analysis_start = Column(Float)
    t_analysis_end = Column(Float)
    t_sse_delivered = Column(Float)
    image_size_bytes = Column(Integer)
    jpeg_size_bytes = Column(Integer)
    success = Column(Boolean, default=False)
    error = Column(String(512), nullable=True)
    created_at = Column(DateTime, default=datetime.utcnow)


class EnergyTelemetry(Base):
    """Per-window energy phase-timer telemetry from the board (status:"energy").

    The firmware publishes {window_ms, ps_rest_ms, capture_ms} once per minute
    while in WIFI_PS_REST. Aggregated to compute the MEASURED duty cycle for RQ3
    (replaces the assumed 7s/30min duty cycle with real operational data).
    """

    __tablename__ = "energy_telemetry"

    id = Column(Integer, primary_key=True, autoincrement=True)
    window_ms = Column(Integer, nullable=False)
    ps_rest_ms = Column(Integer, nullable=False)
    capture_ms = Column(Integer, nullable=False)
    received_at = Column(DateTime, default=datetime.utcnow, index=True)
