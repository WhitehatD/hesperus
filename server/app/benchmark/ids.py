"""Monotonic task_id counter — shared by REST and agent capture paths.

task_id is uint32_t in firmware (firmware/Core/Src/main.c). The counter is
in-process, so it is seeded at startup (app/main.py lifespan) from the
highest task_id already stored. Before 2026-11-12 it always restarted at 1:
after a restart new captures reused task_ids that old photos and analyses
already had, and anything looked up by task_id (the gallery's analysis
overlay, GET /api/analysis/{task_id}) mixed them up. A phone photo was shown
with an August analysis of a dark board frame because both were task #1.

Keep it below 10000: /api/upload treats 10000-40000 as board fallback ids
and replaces them.
"""
import itertools

_counter = itertools.count(1)


def seed_task_ids(highest_used: int) -> None:
    """Continue numbering after *highest_used*."""
    global _counter
    _counter = itertools.count(max(0, highest_used) + 1)


def next_task_id() -> int:
    """Return the next task_id (monotonic, never repeats in-process)."""
    return next(_counter)
