"""
Resumable chunked upload — /api/upload/chunk, /api/upload/resume,
/api/upload/complete.

2026-08-19: added alongside the fix for uploads failing over a lossy 4G
uplink. A single monolithic POST loses the whole 614KB frame to one bad
packet; this endpoint set lets the firmware resume from the last
server-confirmed byte offset instead of restarting from zero.
"""
import io

from unittest.mock import patch

import pytest

from app.api.firmware_routes import _compute_crc32


TOKEN = "secret-board-token-2026"


def _auth_settings(mock_settings, tmp_path):
    mock_settings.firmware_upload_token = TOKEN
    mock_settings.upload_dir = str(tmp_path)


def test_chunked_upload_happy_path(client, tmp_path):
    """Two chunks, in order, then complete — should match /api/upload's
    behavior end to end (200, correct task_id, image processed)."""
    payload = bytes((i % 256 for i in range(50_000)))
    crc = _compute_crc32(payload)
    chunk1, chunk2 = payload[:30_000], payload[30_000:]

    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}

        r1 = client.post(
            "/api/upload/chunk",
            params={"task_id": 501, "offset": 0, "total_size": len(payload)},
            content=chunk1,
            headers=headers,
        )
        assert r1.status_code == 200
        assert r1.json() == {"task_id": 501, "received_offset": 30_000, "total_size": 50_000}

        r2 = client.post(
            "/api/upload/chunk",
            params={"task_id": 501, "offset": 30_000, "total_size": len(payload)},
            content=chunk2,
            headers=headers,
        )
        assert r2.status_code == 200
        assert r2.json()["received_offset"] == 50_000

        rc = client.post(
            "/api/upload/complete",
            params={"task_id": 501, "total_size": len(payload), "crc32": crc},
            headers=headers,
        )
        assert rc.status_code == 200
        assert rc.json()["task_id"] == 501

        # temp files cleaned up
        assert not (tmp_path / ".incomplete" / "task_501.tmp").exists()
        assert not (tmp_path / ".incomplete" / "task_501.offset").exists()


def test_chunk_requires_auth(client, tmp_path):
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        resp = client.post(
            "/api/upload/chunk",
            params={"task_id": 1, "offset": 0, "total_size": 100},
            content=b"x" * 100,
        )
        assert resp.status_code == 403


def test_resume_reports_progress_mid_upload(client, tmp_path):
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}

        # No upload started yet
        r0 = client.get("/api/upload/resume", params={"task_id": 502})
        assert r0.json() == {"task_id": 502, "exists": False, "received_offset": 0}

        client.post(
            "/api/upload/chunk",
            params={"task_id": 502, "offset": 0, "total_size": 40_000},
            content=b"a" * 25_000,
            headers=headers,
        )

        r1 = client.get("/api/upload/resume", params={"task_id": 502})
        assert r1.json() == {"task_id": 502, "exists": True, "received_offset": 25_000}


def test_duplicate_chunk_returns_current_offset_without_corrupting(client, tmp_path):
    """A retried chunk whose ack was lost, then the retry ALSO lands, must
    not double-write or desync the tracked offset."""
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}

        client.post(
            "/api/upload/chunk",
            params={"task_id": 503, "offset": 0, "total_size": 20_000},
            content=b"a" * 10_000,
            headers=headers,
        )
        # Same chunk again (offset=0 while server is already at 10000)
        dup = client.post(
            "/api/upload/chunk",
            params={"task_id": 503, "offset": 0, "total_size": 20_000},
            content=b"a" * 10_000,
            headers=headers,
        )
        assert dup.status_code == 200
        assert dup.json()["received_offset"] == 10_000  # unchanged, not re-applied

        r = client.get("/api/upload/resume", params={"task_id": 503})
        assert r.json()["received_offset"] == 10_000


def test_continuation_chunk_for_unknown_upload_returns_409(client, tmp_path):
    """offset > 0 for a task_id the server has no record of (e.g. server
    restarted, or the sidecar was swept) must tell the board to restart."""
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}

        resp = client.post(
            "/api/upload/chunk",
            params={"task_id": 999999, "offset": 5000, "total_size": 20_000},
            content=b"a" * 5000,
            headers=headers,
        )
        assert resp.status_code == 409


def test_complete_before_fully_received_returns_409(client, tmp_path):
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}

        client.post(
            "/api/upload/chunk",
            params={"task_id": 504, "offset": 0, "total_size": 20_000},
            content=b"a" * 10_000,
            headers=headers,
        )
        resp = client.post(
            "/api/upload/complete",
            params={"task_id": 504, "total_size": 20_000, "crc32": 0},
            headers=headers,
        )
        assert resp.status_code == 409


def test_complete_with_wrong_crc_returns_422_and_preserves_file(client, tmp_path):
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}
        payload = b"x" * 10_000

        client.post(
            "/api/upload/chunk",
            params={"task_id": 505, "offset": 0, "total_size": len(payload)},
            content=payload,
            headers=headers,
        )
        resp = client.post(
            "/api/upload/complete",
            params={"task_id": 505, "total_size": len(payload), "crc32": 12345},
            headers=headers,
        )
        assert resp.status_code == 422
        # File preserved for retry/inspection, not silently deleted
        assert (tmp_path / ".incomplete" / "task_505.tmp").exists()


def test_fallback_task_id_remapped_only_on_first_chunk(client, tmp_path):
    """task_id=0 (unprompted/button capture marker) must be remapped to a
    real ID on offset=0, and that resolved ID returned so the firmware uses
    it for every subsequent chunk of the SAME upload."""
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}

        r1 = client.post(
            "/api/upload/chunk",
            params={"task_id": 0, "offset": 0, "total_size": 100},
            content=b"a" * 100,
            headers=headers,
        )
        assert r1.status_code == 200
        resolved_id = r1.json()["task_id"]
        assert resolved_id != 0

        # A second offset=0 chunk with task_id=0 must resolve to a
        # DIFFERENT id (each new upload gets a fresh one) — confirming
        # remap only happens once per upload, keyed by the chunk sequence,
        # not by accident reusing state.
        r2 = client.post(
            "/api/upload/chunk",
            params={"task_id": 0, "offset": 0, "total_size": 100},
            content=b"b" * 100,
            headers=headers,
        )
        assert r2.json()["task_id"] != resolved_id


def test_invalid_offset_and_total_size_rejected(client, tmp_path):
    with patch("app.api.routes.settings") as mock_settings:
        _auth_settings(mock_settings, tmp_path)
        headers = {"X-Upload-Token": TOKEN}

        bad_total = client.post(
            "/api/upload/chunk",
            params={"task_id": 1, "offset": 0, "total_size": 0},
            content=b"a",
            headers=headers,
        )
        assert bad_total.status_code == 422

        bad_offset = client.post(
            "/api/upload/chunk",
            params={"task_id": 1, "offset": -1, "total_size": 100},
            content=b"a",
            headers=headers,
        )
        assert bad_offset.status_code == 422

        overflow = client.post(
            "/api/upload/chunk",
            params={"task_id": 1, "offset": 0, "total_size": 10},
            content=b"a" * 20,
            headers=headers,
        )
        assert overflow.status_code == 422
