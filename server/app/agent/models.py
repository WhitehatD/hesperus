"""
Thesis IoT Server — Agent Chat Session Models
Persistent chat sessions and messages per board device.
"""

from datetime import datetime

from sqlalchemy import DateTime, ForeignKey, Integer, String, Text, func
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.db.database import Base


class ChatSession(Base):
    """A chat session between a user and the monitoring agent for a specific board."""

    __tablename__ = "chat_sessions"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    board_id: Mapped[str] = mapped_column(String(128), nullable=False, index=True)
    name: Mapped[str] = mapped_column(String(256), nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now())

    messages: Mapped[list["ChatMessage"]] = relationship(
        "ChatMessage", back_populates="session", cascade="all, delete-orphan",
        order_by="ChatMessage.created_at",
    )


class ChatMessage(Base):
    """A single message in an agent chat session."""

    __tablename__ = "chat_messages"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    session_id: Mapped[int] = mapped_column(Integer, ForeignKey("chat_sessions.id", ondelete="CASCADE"), nullable=False, index=True)
    role: Mapped[str] = mapped_column(String(16), nullable=False)  # "user" or "assistant"
    content: Mapped[str] = mapped_column(Text, nullable=False)
    # 2026-08-24: JSON-serialized list of the SAME block shape the dashboard's
    # live SSE stream renders (step/text/error, matching AgentChat.tsx's
    # Block type) — NULL for user messages and for assistant rows written
    # before this column existed. Without this, a page refresh only ever saw
    # `content` (the model's own terse "1-2 sentence" follow-up per the
    # system prompt's CONCISE rule), never the rich capture/analysis/image
    # cards the user actually watched stream in live — those blocks only
    # ever existed in the browser's in-memory React state. See
    # _build_assistant_blocks() in agent_routes.py for how this is built.
    blocks_json: Mapped[str | None] = mapped_column(Text, nullable=True, default=None)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now())

    session: Mapped["ChatSession"] = relationship("ChatSession", back_populates="messages")
