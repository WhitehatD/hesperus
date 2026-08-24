"""
Thesis IoT Server — Async Database Engine
SQLAlchemy async engine + session factory for SQLite.
"""

from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase

from app.config import settings


engine = create_async_engine(
    settings.database_url,
    echo=settings.debug,
    # timeout: seconds aiosqlite waits before raising OperationalError on lock
    connect_args={"check_same_thread": False, "timeout": 30},
)

async_session = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)


class Base(DeclarativeBase):
    """Declarative base for all ORM models."""
    pass


async def create_tables():
    """Create all tables that don't exist yet, then run lightweight migrations."""
    import app.db.wifi_models  # noqa: F401 — register WiFi config model
    import app.analysis.models  # noqa: F401 — register analysis result + CaptureLatency models
    import app.scheduler.models  # noqa: F401 — register scheduler models
    import app.agent.models  # noqa: F401 — register agent chat models
    async with engine.begin() as conn:
        import sqlalchemy as sa
        # WAL mode: concurrent readers + writer (prevents poll SELECT blocking behind
        # _run_analysis INSERT/COMMIT). busy_timeout: retry on lock for up to 5s.
        await conn.execute(sa.text("PRAGMA journal_mode=WAL"))
        await conn.execute(sa.text("PRAGMA busy_timeout=5000"))
        await conn.run_sync(Base.metadata.create_all)
        # Lightweight column migrations for SQLite (ALTER TABLE ADD COLUMN is safe)
        await conn.run_sync(_migrate_columns)


def _migrate_columns(conn):
    """Add missing columns to existing tables (idempotent)."""
    import sqlalchemy as sa
    insp = sa.inspect(conn)
    if insp.has_table("schedule_tasks"):
        cols = [c["name"] for c in insp.get_columns("schedule_tasks")]
        if "completed_at" not in cols:
            conn.execute(sa.text("ALTER TABLE schedule_tasks ADD COLUMN completed_at DATETIME"))
            print("[MIGRATE] Added schedule_tasks.completed_at")
    if insp.has_table("analysis_results"):
        cols = [c["name"] for c in insp.get_columns("analysis_results")]
        if "flagged" not in cols:
            conn.execute(sa.text("ALTER TABLE analysis_results ADD COLUMN flagged BOOLEAN DEFAULT 0 NOT NULL"))
            print("[MIGRATE] Added analysis_results.flagged")
        if "flag_reason" not in cols:
            conn.execute(sa.text("ALTER TABLE analysis_results ADD COLUMN flag_reason TEXT DEFAULT ''"))
            print("[MIGRATE] Added analysis_results.flag_reason")
    if insp.has_table("chat_messages"):
        cols = [c["name"] for c in insp.get_columns("chat_messages")]
        if "blocks_json" not in cols:
            conn.execute(sa.text("ALTER TABLE chat_messages ADD COLUMN blocks_json TEXT"))
            print("[MIGRATE] Added chat_messages.blocks_json")


async def get_db():
    """FastAPI dependency — yields an async DB session."""
    async with async_session() as session:
        yield session
