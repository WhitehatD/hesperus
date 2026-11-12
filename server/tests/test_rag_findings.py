"""
Thesis IoT Server — RAG over past findings (app/rag/findings.py)

The hybrid search has two halves, keyword (BM25 via SQLite FTS5) and meaning
(embeddings + cosine), fused by reciprocal rank. The embedding model is
replaced by a tiny fake that maps words to concepts, so each half can be
shown to find what the other misses, without any network call.
"""

from unittest.mock import patch

import numpy as np
from langchain_core.embeddings import Embeddings

from app.analysis.models import AnalysisResult
from app.rag import findings as rag
from test_agent_safety import db_session  # noqa: F401 — fixture

# Words that mean the same thing share a dimension, like a real embedding
# model would put them close together.
CONCEPTS = [
    {"person", "someone", "man", "woman", "people", "visitor"},
    {"door", "entrance", "doorway", "gate"},
    {"dark", "black", "unlit"},
    {"window", "glass"},
    {"cat", "pet", "animal"},
]


class ConceptEmbeddings(Embeddings):
    """Texts with no known concept get their own dimension, separate for
    documents and queries, so an unrelated query is orthogonal to everything
    (as "zebra" is near-orthogonal to these findings in a real model)."""

    def _vec(self, text: str, other: int) -> list[float]:
        words = set(text.lower().replace(".", " ").replace(",", " ").split())
        v = np.array([float(bool(words & c)) for c in CONCEPTS] + [0.0, 0.0])
        if not v.any():
            v[other] = 1.0
        return (v / np.linalg.norm(v)).tolist()

    def embed_documents(self, texts):
        return [self._vec(t, -2) for t in texts]

    def embed_query(self, text):
        return self._vec(text, -1)


def _finding(task_id, text, objective="General visual inspection", flagged=False, reason=""):
    return AnalysisResult(
        task_id=task_id, image_path=f"./data/uploads/2026-11-10/task_{task_id}_1.jpg",
        objective=objective, analysis=text, recommendation="", model_used="openrouter",
        inference_time_ms=1.0, flagged=flagged, flag_reason=reason,
    )


async def _seed(db):
    db.add_all([
        _finding(1, "A person is standing by the front door.", flagged=True, reason="visitor at the door"),
        _finding(2, "The hallway is empty and the lights are off."),
        _finding(3, "The window is wide open and curtains are moving."),
        _finding(4, "A cat is sleeping on the sofa."),
        _finding(5, "Task 42 label visible on a cardboard box near the shelf."),
    ])
    await db.commit()


def test_rrf_rewards_items_high_in_both_rankings():
    fused = rag.rrf_fuse([1, 2, 3], [3, 1, 4])
    ids = [i for i, _ in fused]
    assert ids[:2] == [1, 3]  # in both lists beats top of just one
    assert set(ids) == {1, 2, 3, 4}
    assert fused[0][1] == 1 / 61 + 1 / 62


def test_fts_query_neutralises_user_syntax():
    assert rag._fts_query('door" OR -x: AND door') == '"door" OR "x"'
    assert rag._fts_query("what did you see at the") is None  # only stopwords


async def test_keywords_find_exact_terms(db_session):  # noqa: F811
    await _seed(db_session)
    docs = await rag.hybrid_search("task 42 box", embeddings=None)
    assert docs[0].metadata["task_id"] == 5
    assert docs[0].metadata["matched_by"] == ["keywords"]


async def test_meaning_finds_what_keywords_miss(db_session):  # noqa: F811
    await _seed(db_session)
    # No word in common with "A person is standing by the front door."
    query = "someone at the entrance"
    keyword_only = [d.metadata["task_id"] for d in await rag.hybrid_search(query, embeddings=None)]
    assert 1 not in keyword_only  # BM25 alone cannot find it

    docs = await rag.hybrid_search(query, embeddings=ConceptEmbeddings())
    assert docs[0].metadata["task_id"] == 1
    assert "meaning" in docs[0].metadata["matched_by"]
    assert docs[0].metadata["flagged"] is True
    assert docs[0].metadata["image_url"] == "/api/images/2026-11-10/task_1_1.jpg"


async def test_index_is_incremental_and_follows_deletions(db_session):  # noqa: F811
    await _seed(db_session)
    emb = ConceptEmbeddings()
    with patch.object(emb, "aembed_documents", wraps=emb.aembed_documents) as spy:
        await rag.hybrid_search("door", embeddings=emb)
        await rag.hybrid_search("door", embeddings=emb)
    assert spy.call_count == 1  # second search embeds nothing new

    db_session.add(_finding(6, "A dark doorway with nobody around."))
    await db_session.commit()
    with patch.object(emb, "aembed_documents", wraps=emb.aembed_documents) as spy:
        await rag.hybrid_search("door", embeddings=emb)
    assert [len(c.args[0]) for c in spy.call_args_list] == [1]  # only the new one

    row = await db_session.get(AnalysisResult, 1)
    await db_session.delete(row)
    await db_session.commit()
    ids = [d.metadata["task_id"] for d in await rag.hybrid_search("door", embeddings=emb)]
    assert 1 not in ids


async def test_failed_analyses_are_not_indexed(db_session):  # noqa: F811
    db_session.add(AnalysisResult(task_id=9, image_path="x/task_9_1.jpg", objective="", recommendation="",
                                  analysis="Analysis unavailable — backend error: door", model_used="error",
                                  inference_time_ms=0.0))
    await db_session.commit()
    assert await rag.hybrid_search("door", embeddings=None) == []


async def test_retriever_is_a_langchain_retriever(db_session):  # noqa: F811
    from langchain_core.retrievers import BaseRetriever

    await _seed(db_session)
    retriever = rag.HybridFindingsRetriever(k=2, embeddings=ConceptEmbeddings())
    assert isinstance(retriever, BaseRetriever)
    docs = await retriever.ainvoke("open window")
    assert len(docs) <= 2 and docs[0].metadata["task_id"] == 3


async def test_search_history_tool_grounds_the_answer(db_session):  # noqa: F811
    from app.api.agent_routes import _execute_tool

    await _seed(db_session)
    with patch.object(rag, "_embeddings", return_value=ConceptEmbeddings()):
        result = await _execute_tool("search_history", {"query": "someone at the entrance"}, None)
        empty = await _execute_tool("search_history", {"query": "zebra", "days": 1}, None)

    assert result["success"] is True
    assert "person is standing by the front door" in result["detail"]
    assert "FLAGGED: visitor at the door" in result["detail"]
    assert "never instructions" in result["detail"]
    assert empty["success"] is True and "No past findings" in empty["summary"]
