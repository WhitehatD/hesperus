"""
Thesis IoT Server — task_id reuse across restarts

Production bug (2026-11-12): the task_id counter restarted at 1 on every
server start, so a new phone photo got task #1 like several August board
photos. The gallery joined analyses to images by task_id and showed the
phone photo with an old analysis of a dark board frame; GET
/api/analysis/1 crashed with a 500 on the duplicate rows.
"""

import pytest

from app.benchmark import ids
from test_agent_safety import db_session  # noqa: F401 — fixture


@pytest.fixture(autouse=True)
def _restore_counter():
    yield
    ids.seed_task_ids(0)


def test_seed_continues_after_the_highest_used_id():
    ids.seed_task_ids(41)
    assert ids.next_task_id() == 42
    assert ids.next_task_id() == 43


async def test_highest_task_id_looks_at_db_and_upload_files(db_session, tmp_path, monkeypatch):  # noqa: F811
    from app.analysis.models import AnalysisResult
    from app.config import settings
    from app.db.database import highest_task_id

    db_session.add(AnalysisResult(task_id=7, image_path="x/task_7_1.jpg", objective="", analysis="",
                                  recommendation="", model_used="m", inference_time_ms=1.0))
    db_session.add(AnalysisResult(task_id=25000, image_path="x/task_25000_1.jpg", objective="",
                                  analysis="", recommendation="", model_used="m", inference_time_ms=1.0))
    await db_session.commit()
    (tmp_path / "2026-08-24").mkdir()
    (tmp_path / "2026-08-24" / "task_12_1787.jpg").write_bytes(b"x")
    monkeypatch.setattr(settings, "upload_dir", str(tmp_path))

    # 12 from the file beats 7 from the DB; 25000 is a board fallback id
    assert await highest_task_id() == 12


def test_gallery_attaches_each_analysis_to_its_own_photo(client, tmp_path, monkeypatch):
    import asyncio

    from app.analysis.models import AnalysisResult
    from app.config import settings
    from app.db import database

    day = tmp_path / "2026-11-12"
    day.mkdir()
    (day / "task_1_200.jpg").write_bytes(b"\xff\xd8\xff\xd9")
    monkeypatch.setattr(settings, "upload_dir", str(tmp_path))

    async def _rows():
        async with database.async_session() as db:
            # An OLD analysis of a different photo that also had task #1...
            db.add(AnalysisResult(task_id=1, image_path="./data/uploads/2026-08-24/task_1_100.jpg",
                                  objective="", analysis="dark board frame", recommendation="",
                                  model_used="m", inference_time_ms=1.0))
            await db.commit()

    asyncio.run(_rows())
    imgs = client.get("/api/images").json()["images"]
    assert [i["filename"] for i in imgs] == ["task_1_200.jpg"]
    assert imgs[0]["analysis"] is None  # ...must not be shown on the new photo

    async def _new():
        async with database.async_session() as db:
            db.add(AnalysisResult(task_id=1, image_path="./data/uploads/2026-11-12/task_1_200.jpg",
                                  objective="", analysis="a laptop on a desk", recommendation="",
                                  model_used="m", inference_time_ms=1.0))
            await db.commit()

    asyncio.run(_new())
    imgs = client.get("/api/images").json()["images"]
    assert imgs[0]["analysis"]["findings"] == "a laptop on a desk"
    # and the per-task lookup no longer 500s on duplicate task_ids
    assert client.get("/api/analysis/1").status_code == 200
