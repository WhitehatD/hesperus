"""
Thesis IoT Server — RAG over the camera's past findings

Every photo the camera takes is analysed by the vision model and stored as
an AnalysisResult (objective, findings, recommendation, flag). This module
makes that history searchable, so the agent can answer "what did the camera
see in the hallway last week?" from what was actually observed instead of
guessing. The agent calls it through the `search_history` tool; the
retrieved findings go back into the conversation and the model answers from
them. That is retrieval-augmented generation (RAG), with the agent deciding
when to retrieve.

Hybrid retrieval: two independent rankings of the same findings, fused.

  1. BM25 keyword ranking: SQLite FTS5 full-text index (findings_fts).
     Good at exact words: "door", "task 42", a name written on a sign.
  2. Embedding ranking: each finding is turned into a vector by an
     embedding model (OpenRouter, settings.rag_embedding_model) and stored
     in findings_vectors; the query is embedded the same way and compared
     by cosine similarity. Good at meaning: "someone at the entrance"
     finds "a person standing by the front door".
  3. Reciprocal rank fusion (RRF): score = sum over both lists of
     1 / (RRF_K + rank). Only ranks are used, so the two very different
     score scales (BM25 vs cosine) never have to be calibrated against each
     other, and a finding near the top of both lists wins.

No separate vector database: a few thousand findings fit in one numpy
matrix, and keeping everything in the existing SQLite file means no new
service to deploy. The index is updated incrementally before each search
(only findings that are not indexed yet are embedded), so the three places
that write AnalysisResult rows need no changes.

LangChain: HybridFindingsRetriever is a LangChain BaseRetriever returning
Documents, and embeddings use LangChain's OpenAIEmbeddings interface
(app/llm_clients.get_embeddings), so either half can be swapped for any
other LangChain retriever or embedding model.

Without an OpenRouter key the search falls back to BM25 only.
"""

import re
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
import sqlalchemy as sa
from langchain_core.callbacks import AsyncCallbackManagerForRetrieverRun, CallbackManagerForRetrieverRun
from langchain_core.documents import Document
from langchain_core.embeddings import Embeddings
from langchain_core.retrievers import BaseRetriever

from app.config import settings

RRF_K = 60  # standard constant from the original RRF paper (Cormack et al., 2009)
CANDIDATES = 20  # how deep each ranking goes before fusion
# Cosine floor for the embedding ranking. Without it the meaning half always
# returns its 20 closest findings, so "zebra" "found" five. Measured with
# text-embedding-3-small on 2026-11-12: relevant query/finding pairs scored
# 0.39-0.56, unrelated queries ("zebra", "recipe for pancakes") at most 0.15.
MIN_SIMILARITY = 0.25
EMBED_BATCH = 64

_SCHEMA = (
    "CREATE VIRTUAL TABLE IF NOT EXISTS findings_fts USING fts5("
    " text, analysis_id UNINDEXED, tokenize='porter unicode61')",
    "CREATE TABLE IF NOT EXISTS findings_vectors ("
    " analysis_id INTEGER PRIMARY KEY, model TEXT NOT NULL, vector BLOB NOT NULL)",
)


def _embeddings() -> Embeddings | None:
    if not settings.openrouter_api_key:
        return None
    from app.llm_clients import get_embeddings
    return get_embeddings()


def document_text(objective: str, findings: str, recommendation: str, flag_reason: str) -> str:
    """What gets indexed for one analysis, both for BM25 and the embedding."""
    parts = [f"Objective: {objective}", f"Findings: {findings}"]
    if recommendation:
        parts.append(f"Recommendation: {recommendation}")
    if flag_reason:
        parts.append(f"Flagged: {flag_reason}")
    return "\n".join(parts)


def rrf_fuse(*rankings: list[int], k: int = RRF_K) -> list[tuple[int, float]]:
    """Reciprocal rank fusion. Each ranking is a list of ids, best first.
    Returns (id, score) best first. Ranks start at 1."""
    scores: dict[int, float] = {}
    for ranking in rankings:
        for rank, item in enumerate(ranking, start=1):
            scores[item] = scores.get(item, 0.0) + 1.0 / (k + rank)
    return sorted(scores.items(), key=lambda kv: kv[1], reverse=True)


# FTS5 has no stopword list; without this every query matched every finding
# on "the" / "is" / "at".
_STOPWORDS = frozenset(
    "a an the and or but of at in on to for from by with as is are was were be been being "
    "it its this that these those there here what when where which who whom how did do does "
    "has have had any anything something someone some all no not you your i me my we our "
    "camera see saw seen look looked show last ago time".split()
)


def _fts_query(text: str) -> str | None:
    """User text -> a safe FTS5 query: content words quoted, OR-ed together.
    Raw user text would hit FTS5 syntax (quotes, '-', 'AND', ':')."""
    words = [w for w in re.findall(r"\w+", text.lower()) if w not in _STOPWORDS]
    return " OR ".join(f'"{w}"' for w in dict.fromkeys(words)) or None


async def ensure_index(db, embeddings: Embeddings | None) -> int:
    """Index every analysis not indexed yet. Returns how many were added."""
    for stmt in _SCHEMA:
        await db.execute(sa.text(stmt))
    # Analyses deleted since the last search (delete_image) leave the index.
    await db.execute(sa.text(
        "DELETE FROM findings_fts WHERE analysis_id NOT IN (SELECT id FROM analysis_results)"))
    await db.execute(sa.text(
        "DELETE FROM findings_vectors WHERE analysis_id NOT IN (SELECT id FROM analysis_results)"))

    rows = (await db.execute(sa.text(
        "SELECT id, objective, analysis, recommendation, flag_reason FROM analysis_results "
        "WHERE model_used != 'error' AND id NOT IN (SELECT analysis_id FROM findings_fts)"
    ))).all()
    for r in rows:
        await db.execute(
            sa.text("INSERT INTO findings_fts (text, analysis_id) VALUES (:t, :id)"),
            {"t": document_text(r.objective or "", r.analysis or "", r.recommendation or "", r.flag_reason or ""),
             "id": r.id},
        )

    added_vectors = 0
    if embeddings is not None:
        model = settings.rag_embedding_model
        # A different embedding model makes old vectors incomparable: redo them.
        await db.execute(sa.text("DELETE FROM findings_vectors WHERE model != :m"), {"m": model})
        missing = (await db.execute(sa.text(
            "SELECT f.analysis_id, f.text FROM findings_fts f "
            "WHERE f.analysis_id NOT IN (SELECT analysis_id FROM findings_vectors)"
        ))).all()
        for i in range(0, len(missing), EMBED_BATCH):
            batch = missing[i:i + EMBED_BATCH]
            vectors = await embeddings.aembed_documents([b.text for b in batch])
            for b, v in zip(batch, vectors):
                await db.execute(
                    sa.text("INSERT INTO findings_vectors (analysis_id, model, vector) VALUES (:id, :m, :v)"),
                    {"id": int(b.analysis_id), "m": model, "v": np.asarray(v, dtype=np.float32).tobytes()},
                )
            added_vectors += len(batch)

    await db.commit()
    return max(len(rows), added_vectors)


async def _bm25_ranking(db, query: str, allowed: set[int] | None) -> list[int]:
    q = _fts_query(query)
    if not q:
        return []
    rows = (await db.execute(sa.text(
        "SELECT analysis_id FROM findings_fts WHERE findings_fts MATCH :q ORDER BY bm25(findings_fts) LIMIT :n"
    ), {"q": q, "n": CANDIDATES * 3})).all()
    ids = [int(r.analysis_id) for r in rows]
    if allowed is not None:
        ids = [i for i in ids if i in allowed]
    return ids[:CANDIDATES]


async def _vector_ranking(db, query: str, embeddings: Embeddings, allowed: set[int] | None) -> list[int]:
    rows = (await db.execute(sa.text(
        "SELECT analysis_id, vector FROM findings_vectors WHERE model = :m"
    ), {"m": settings.rag_embedding_model})).all()
    if allowed is not None:
        rows = [r for r in rows if int(r.analysis_id) in allowed]
    if not rows:
        return []
    matrix = np.vstack([np.frombuffer(r.vector, dtype=np.float32) for r in rows])
    q = np.asarray(await embeddings.aembed_query(query), dtype=np.float32)
    sims = matrix @ q / (np.linalg.norm(matrix, axis=1) * np.linalg.norm(q) + 1e-9)
    order = [i for i in np.argsort(-sims)[:CANDIDATES] if sims[i] >= MIN_SIMILARITY]
    return [int(rows[i].analysis_id) for i in order]


async def hybrid_search(query: str, k: int = 5, days: int | None = None,
                        embeddings: Embeddings | None = None) -> list[Document]:
    """BM25 + embeddings, fused with RRF. Returns up to k Documents, best first."""
    from app.db.database import async_session  # local: tests re-point it

    async with async_session() as db:
        await ensure_index(db, embeddings)

        allowed = None
        if days:
            cutoff = datetime.utcnow() - timedelta(days=days)  # created_at is UTC
            allowed = {int(r.id) for r in (await db.execute(sa.text(
                "SELECT id FROM analysis_results WHERE created_at >= :c"), {"c": cutoff})).all()}

        bm25 = await _bm25_ranking(db, query, allowed)
        vector = await _vector_ranking(db, query, embeddings, allowed) if embeddings else []
        fused = rrf_fuse(bm25, vector)[:k]
        if not fused:
            return []

        ids = [i for i, _ in fused]
        rows = {int(r.id): r for r in (await db.execute(sa.text(
            "SELECT id, task_id, image_path, objective, analysis, recommendation, flagged, flag_reason, created_at "
            "FROM analysis_results WHERE id IN (" + ",".join(str(i) for i in ids) + ")"
        ))).all()}

    docs = []
    for analysis_id, score in fused:
        r = rows.get(analysis_id)
        if r is None:
            continue
        path = Path(r.image_path or "")
        docs.append(Document(
            page_content=r.analysis or "",
            metadata={
                "analysis_id": analysis_id,
                "task_id": r.task_id,
                "objective": r.objective or "",
                "recommendation": r.recommendation or "",
                "flagged": bool(r.flagged),
                "flag_reason": r.flag_reason or "",
                "created_at": str(r.created_at)[:16],
                "image_url": f"/api/images/{path.parent.name}/{path.name}" if path.name else "",
                "rrf_score": round(score, 5),
                "matched_by": [name for name, ranking in (("keywords", bm25), ("meaning", vector))
                               if analysis_id in ranking],
            },
        ))
    return docs


class HybridFindingsRetriever(BaseRetriever):
    """LangChain retriever over the camera's findings history (see module
    docstring). `await HybridFindingsRetriever(k=5).ainvoke("door open")`."""

    k: int = 5
    days: int | None = None
    embeddings: Embeddings | None = None

    async def _aget_relevant_documents(
        self, query: str, *, run_manager: AsyncCallbackManagerForRetrieverRun,
    ) -> list[Document]:
        return await hybrid_search(query, k=self.k, days=self.days, embeddings=self.embeddings)

    def _get_relevant_documents(
        self, query: str, *, run_manager: CallbackManagerForRetrieverRun,
    ) -> list[Document]:
        raise NotImplementedError("async only: use ainvoke")


def default_retriever(k: int = 5, days: int | None = None) -> HybridFindingsRetriever:
    return HybridFindingsRetriever(k=k, days=days, embeddings=_embeddings())
