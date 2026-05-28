"""
Tests for the benchmark infrastructure:
  - CaptureLatency table creation
  - GET /api/benchmark/latency endpoint
"""

import pytest


def test_capture_latency_table_exists(client):
    """CaptureLatency table should be created on startup.

    Verifies that the ORM model is registered and Base.metadata.create_all
    includes the capture_latency table.  We do this by inspecting the DB
    via SQLAlchemy's reflection API through the FastAPI lifespan.
    """
    from app.analysis.models import CaptureLatency
    from app.db.database import Base

    # Table must be present in the metadata registry
    assert "capture_latency" in Base.metadata.tables, (
        "capture_latency table not registered in SQLAlchemy metadata"
    )

    # All expected columns must exist
    table = Base.metadata.tables["capture_latency"]
    expected_columns = {
        "id", "task_id", "run_id", "capture_type", "model_key",
        "t_request", "t_plan_start", "t_plan_end", "t_mqtt_sent",
        "t_upload_received", "t_jpeg_converted", "t_analysis_start",
        "t_analysis_end", "t_sse_delivered",
        "image_size_bytes", "jpeg_size_bytes",
        "success", "error", "created_at",
    }
    actual_columns = {c.name for c in table.columns}
    missing = expected_columns - actual_columns
    assert not missing, f"CaptureLatency is missing columns: {missing}"


def test_benchmark_latency_endpoint(client):
    """GET /api/benchmark/latency should return 200 with empty rows on a fresh DB."""
    resp = client.get("/api/benchmark/latency")
    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"

    data = resp.json()
    assert "rows" in data, "Response must contain 'rows' key"
    assert isinstance(data["rows"], list), "'rows' must be a list"
    # Fresh in-memory DB has no rows
    assert data["rows"] == [], f"Expected empty rows on fresh DB, got: {data['rows']}"


def test_benchmark_latency_with_limit(client):
    """GET /api/benchmark/latency?limit=10 should respect the limit param."""
    resp = client.get("/api/benchmark/latency?limit=10")
    assert resp.status_code == 200
    data = resp.json()
    assert "rows" in data
    assert len(data["rows"]) <= 10


def test_benchmark_plan_endpoint_reachable(client):
    """POST /api/benchmark/plan should return 422 for empty payload (not 404/500)."""
    resp = client.post("/api/benchmark/plan", json={})
    # 422 = validation error (missing prompt) — endpoint exists and is reachable
    assert resp.status_code == 422, (
        f"Expected 422 (missing prompt), got {resp.status_code}: {resp.text}"
    )


def test_benchmark_analyze_endpoint_reachable(client):
    """POST /api/benchmark/analyze with missing image_path returns 422."""
    resp = client.post("/api/benchmark/analyze", json={})
    assert resp.status_code == 422, (
        f"Expected 422 (missing image_path), got {resp.status_code}: {resp.text}"
    )


def test_benchmark_analyze_missing_file(client):
    """POST /api/benchmark/analyze with nonexistent image returns 404."""
    # Endpoint now requires multipart file upload; no file -> 422
    resp = client.post(
        "/api/benchmark/analyze",
        data={"model_key": "claude-haiku"},
    )
    assert resp.status_code == 422, (
        f"Expected 422 when file is missing, got {resp.status_code}: {resp.text}"
    )


def test_energy_telemetry_table_exists(client):
    """EnergyTelemetry table should be registered on startup (RQ3 telemetry)."""
    from app.db.database import Base

    assert "energy_telemetry" in Base.metadata.tables, (
        "energy_telemetry table not registered in SQLAlchemy metadata"
    )
    table = Base.metadata.tables["energy_telemetry"]
    expected = {"id", "window_ms", "ps_rest_ms", "capture_ms", "received_at"}
    actual = {c.name for c in table.columns}
    missing = expected - actual
    assert not missing, f"EnergyTelemetry missing columns: {missing}"


def test_benchmark_energy_endpoint_empty(client):
    """GET /api/benchmark/energy returns 200 with zeroed aggregates on a fresh DB."""
    resp = client.get("/api/benchmark/energy")
    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
    data = resp.json()
    assert data["windows"] == 0
    assert data["total_window_ms"] == 0
    assert data["total_ps_rest_ms"] == 0
    assert data["total_capture_ms"] == 0
    assert data["total_active_ms"] == 0
    assert data["measured_active_fraction"] is None
    assert data["rows"] == []
