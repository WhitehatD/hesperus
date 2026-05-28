"""Plan-quality rubric scorer.

Reads a benchmark JSONL written by run_benchmark.py and scores every planning
row against a per-prompt expected-property rubric. Outputs:

  - Per-row score record (JSONL): planning_score_<TIMESTAMP>.jsonl
  - Per-(model, prompt) aggregate (markdown table): plan_scores_<TIMESTAMP>.md

Score axes (each 0 or 1):
  1. routed_correctly       Agent emitted create_schedule (binary success)
  2. interval_correct       Generated intervals match expected (within tolerance)
  3. count_correct          Generated task count matches expected
  4. t0_preserved           First task is at/near NOW when prompt says "immediately"
  5. span_correct           Total span matches expected duration
  6. range_correct          All task times fall within expected window (e.g., daylight)

Composite score per call: 0-6.

The rubric is intentionally STRUCTURAL not semantic — it catches the kind of
failures that produce a "valid-looking but operationally wrong" schedule, which
is exactly the failure class observed empirically (t=0 dropping on P10).

Usage:
  python scripts/score_plans.py results/benchmark_<TIMESTAMP>.jsonl
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any


# ────────────────────────────────────────────────────────────────────────────
# Per-prompt rubric definitions
# ────────────────────────────────────────────────────────────────────────────

# Index matches scripts/run_benchmark.py:PLANNING_PROMPTS order.
# For each prompt we encode the expected structural properties of the schedule.
# Tolerances are deliberately loose (20% on intervals) so we measure
# semantic-fidelity failures, not numerical noise from time-rounding.

RUBRICS: list[dict[str, Any]] = [
    {  # P1 — "Monitor the workspace every 30 minutes for the next 4 hours"
        "idx": 0,
        "expected_interval_min": 30,
        "interval_tol_pct": 25,
        "expected_count": 8,
        "count_tol": 1,
        "expected_span_min": 240,
        "span_tol_pct": 25,
        "t0_keyword": False,
    },
    {  # P2 — "Take a photo every hour starting at 9am for 8 hours, stop if nothing changes"
        "idx": 1,
        "expected_interval_min": 60,
        "interval_tol_pct": 25,
        "expected_count": 8,
        "count_tol": 1,
        "expected_span_min": 480,
        "span_tol_pct": 30,
        "t0_keyword": False,
        "expected_window": ("09:00", "17:00"),  # 9am for 8 hours
        "window_tol_min": 60,
    },
    {  # P3 — "Capture images every 15 minutes between 8am and 6pm on weekdays"
        "idx": 2,
        "expected_interval_min": 15,
        "interval_tol_pct": 25,
        "expected_count": 40,
        "count_tol": 4,
        "expected_window": ("08:00", "18:00"),
        "window_tol_min": 30,
        "t0_keyword": False,
    },
    {  # P4 — "Check the plant every 2 hours during daylight hours"
        "idx": 3,
        "expected_interval_min": 120,
        "interval_tol_pct": 25,
        "expected_count": 7,
        "count_tol": 2,
        "expected_window": ("06:00", "21:00"),  # generous daylight band
        "window_tol_min": 0,
        "t0_keyword": False,
    },
    {  # P5 — "Monitor the entrance every 20 minutes and send an alert if someone arrives"
        "idx": 4,
        "expected_interval_min": 20,
        "interval_tol_pct": 25,
        "expected_count": None,  # duration unspecified; accept anything > 3
        "count_tol": None,
        "min_count": 3,
        "t0_keyword": False,
    },
    {  # P6 — "Take 3 photos per hour for the next 2 hours to track lighting changes"
        "idx": 5,
        "expected_interval_min": 20,
        "interval_tol_pct": 30,
        "expected_count": 6,
        "count_tol": 1,
        "expected_span_min": 120,
        "span_tol_pct": 30,
        "t0_keyword": False,
    },
    {  # P7 — "Capture the workspace at 9am, 12pm, 3pm, and 6pm every day"
        "idx": 6,
        "exact_times": ["09:00", "12:00", "15:00", "18:00"],
        "exact_times_tol_min": 5,
        "expected_count": 4,
        "count_tol": 0,
        "t0_keyword": False,
    },
    {  # P8 — "Monitor continuously every 10 minutes for 1 hour to detect any activity"
        "idx": 7,
        "expected_interval_min": 10,
        "interval_tol_pct": 25,
        "expected_count": 6,
        "count_tol": 1,
        "expected_span_min": 60,
        "span_tol_pct": 25,
        "t0_keyword": False,
    },
    {  # P9 — "Schedule daily monitoring at 8am and 5pm for the next week"
        "idx": 8,
        "exact_times": ["08:00", "17:00"],
        "exact_times_tol_min": 10,
        "expected_count": 14,
        "count_tol": 2,
        "t0_keyword": False,
    },
    {  # P10 — "Take a photo immediately and then every 45 minutes for 3 hours"
        "idx": 9,
        "expected_interval_min": 45,
        "interval_tol_pct": 25,
        "expected_count": 5,  # immediate + 4 at 45/90/135/180 min
        "count_tol": 1,
        "expected_span_min": 180,
        "span_tol_pct": 25,
        "t0_keyword": True,  # ← the headline check
    },
    {  # P11 — "Take a photo right now and then capture every 30 minutes for 2 hours"
        "idx": 10,
        "expected_interval_min": 30,
        "interval_tol_pct": 25,
        "expected_count": 5,  # immediate + 4 at 30/60/90/120 min
        "count_tol": 1,
        "expected_span_min": 120,
        "span_tol_pct": 25,
        "t0_keyword": True,  # t=0 probe
    },
    {  # P12 — "Start with a baseline capture, then take one every 1 hour for 4 hours"
        "idx": 11,
        "expected_interval_min": 60,
        "interval_tol_pct": 25,
        "expected_count": 5,  # baseline + 4 hourly
        "count_tol": 1,
        "expected_span_min": 240,
        "span_tol_pct": 25,
        "t0_keyword": True,  # t=0 probe ("baseline capture" = immediate)
    },
    {  # P13 — "Take an immediate snapshot followed by hourly checks for the next 3 hours"
        "idx": 12,
        "expected_interval_min": 60,
        "interval_tol_pct": 25,
        "expected_count": 4,  # immediate + 3 hourly
        "count_tol": 1,
        "expected_span_min": 180,
        "span_tol_pct": 30,
        "t0_keyword": True,  # t=0 probe
    },
]


# ────────────────────────────────────────────────────────────────────────────
# Score helpers
# ────────────────────────────────────────────────────────────────────────────


def parse_time_str(s: str) -> int | None:
    """Convert HH:MM string to minutes-since-midnight. Returns None on failure."""
    if not s or not isinstance(s, str):
        return None
    s = s.strip()
    # Handle "HH:MM" and "H:MM"
    try:
        parts = s.split(":")
        if len(parts) < 2:
            return None
        h = int(parts[0])
        m = int(parts[1])
        return (h % 24) * 60 + m
    except (ValueError, IndexError):
        return None


def compute_intervals(tasks: list[dict]) -> list[int]:
    """Pairwise minute-differences between successive task times.

    Handles midnight crossover by adding 24h when next < previous.
    """
    times = [parse_time_str(t.get("time", "")) for t in tasks]
    times = [t for t in times if t is not None]
    if len(times) < 2:
        return []
    intervals = []
    for prev, nxt in zip(times[:-1], times[1:]):
        diff = nxt - prev
        if diff < 0:
            diff += 24 * 60  # midnight rollover
        intervals.append(diff)
    return intervals


def score_one_plan(rubric: dict, row: dict) -> dict:
    """Score one planning row against its rubric. Returns dict of {axis: 0|1, ...}."""
    score = {
        "routed_correctly": 0,
        "interval_correct": 0,
        "count_correct": 0,
        "t0_preserved": 0,
        "span_correct": 0,
        "range_correct": 0,
    }
    notes: list[str] = []

    # Routing: did the agent call create_schedule?
    if row.get("tool_name") == "create_schedule":
        score["routed_correctly"] = 1
    else:
        notes.append(f"agent did not call create_schedule (got: {row.get('tool_name')})")
        return {"axes": score, "composite": sum(score.values()), "notes": notes}

    schedule = row.get("schedule_json") or []
    if not isinstance(schedule, list) or len(schedule) == 0:
        notes.append("schedule_json missing or empty (chain to engine failed)")
        return {"axes": score, "composite": sum(score.values()), "notes": notes}

    # Count check
    expected_count = rubric.get("expected_count")
    if expected_count is not None:
        tol = rubric.get("count_tol", 1)
        if abs(len(schedule) - expected_count) <= tol:
            score["count_correct"] = 1
        else:
            notes.append(f"count {len(schedule)} not within {tol} of expected {expected_count}")
    elif rubric.get("min_count") is not None:
        if len(schedule) >= rubric["min_count"]:
            score["count_correct"] = 1
        else:
            notes.append(f"count {len(schedule)} below minimum {rubric['min_count']}")
    else:
        score["count_correct"] = 1  # no expectation = pass

    # Interval check
    expected_interval = rubric.get("expected_interval_min")
    if expected_interval is not None:
        intervals = compute_intervals(schedule)
        if intervals:
            tol_frac = rubric.get("interval_tol_pct", 20) / 100.0
            lo, hi = expected_interval * (1 - tol_frac), expected_interval * (1 + tol_frac)
            in_range = [i for i in intervals if lo <= i <= hi]
            if len(in_range) >= max(1, int(0.8 * len(intervals))):
                score["interval_correct"] = 1
            else:
                notes.append(
                    f"intervals {intervals[:5]}... only {len(in_range)}/{len(intervals)} within {lo:.0f}-{hi:.0f}min"
                )
        else:
            notes.append("could not compute intervals (fewer than 2 valid task times)")
    else:
        score["interval_correct"] = 1  # no expectation

    # Span check
    expected_span = rubric.get("expected_span_min")
    if expected_span is not None and len(schedule) >= 2:
        times = [parse_time_str(t.get("time", "")) for t in schedule]
        times = [t for t in times if t is not None]
        if len(times) >= 2:
            span = times[-1] - times[0]
            if span < 0:
                span += 24 * 60
            tol_frac = rubric.get("span_tol_pct", 25) / 100.0
            lo, hi = expected_span * (1 - tol_frac), expected_span * (1 + tol_frac)
            if lo <= span <= hi:
                score["span_correct"] = 1
            else:
                notes.append(f"span {span}min not within {lo:.0f}-{hi:.0f} of expected {expected_span}")
    else:
        score["span_correct"] = 1

    # Range check (window the plan should fit inside)
    window = rubric.get("expected_window")
    if window is not None:
        w_lo = parse_time_str(window[0])
        w_hi = parse_time_str(window[1])
        tol = rubric.get("window_tol_min", 30)
        if w_lo is not None and w_hi is not None:
            inside = 0
            outside_notes = []
            for t in schedule:
                tm = parse_time_str(t.get("time", ""))
                if tm is None:
                    continue
                if (w_lo - tol) <= tm <= (w_hi + tol):
                    inside += 1
                else:
                    outside_notes.append(t.get("time", "?"))
            if inside >= max(1, int(0.8 * len(schedule))):
                score["range_correct"] = 1
            else:
                notes.append(f"only {inside}/{len(schedule)} tasks inside window {window} (tol {tol}min)")
                if outside_notes:
                    notes.append(f"  outside: {outside_notes[:5]}")
    else:
        score["range_correct"] = 1

    # Exact times check (alternative to window)
    exact_times = rubric.get("exact_times")
    if exact_times is not None:
        tol = rubric.get("exact_times_tol_min", 10)
        expected_mins = [parse_time_str(t) for t in exact_times]
        actual_mins = [parse_time_str(t.get("time", "")) for t in schedule]
        actual_mins = [m for m in actual_mins if m is not None]
        matched = 0
        for em in expected_mins:
            if em is None:
                continue
            if any(abs(am - em) <= tol for am in actual_mins):
                matched += 1
        if matched == len(exact_times):
            score["range_correct"] = 1
        else:
            notes.append(f"exact-times match {matched}/{len(exact_times)} (need all)")

    # t=0 preservation check
    if rubric.get("t0_keyword"):
        # The forwarded prompt (tool_input.prompt) should ideally contain "immediately"
        # or similar AND the first task should be at/near "now".
        # Since we don't know the current time at run-time precisely, use a proxy:
        # forwarded prompt contains a t=0 keyword OR first interval < expected/2.
        forwarded = ""
        ti = row.get("tool_input") or {}
        if isinstance(ti, dict):
            forwarded = (ti.get("prompt") or "").lower()
        keyword_preserved = any(
            kw in forwarded for kw in (
                "immediately", "immediate", "right now", "now and then",
                "first capture", "baseline", "snapshot now", "at once",
                "starting now", "right away",
            )
        )
        first_interval_short = False
        intervals = compute_intervals(schedule)
        if intervals and rubric.get("expected_interval_min"):
            # If the model preserved t=0, the FIRST interval should be smaller than the
            # rest (e.g., first capture at NOW, then 45min). A "missing t=0" plan has
            # uniform intervals matching expected.
            first_interval_short = intervals[0] < rubric["expected_interval_min"] * 0.6

        if keyword_preserved or first_interval_short:
            score["t0_preserved"] = 1
        else:
            notes.append(
                f"t=0 lost: 'immediately' not in forwarded prompt={forwarded!r}; "
                f"first interval={intervals[0] if intervals else '?'}min"
            )

    composite = sum(score.values())
    return {"axes": score, "composite": composite, "notes": notes}


# ────────────────────────────────────────────────────────────────────────────
# Aggregation + markdown rendering
# ────────────────────────────────────────────────────────────────────────────


def render_markdown(rows: list[dict]) -> str:
    """Build a per-(model, prompt) aggregate markdown table.

    Each cell shows mean ± stdev of the composite score across reps.
    """
    by_model_prompt: dict[tuple[str, int], list[int]] = defaultdict(list)
    by_model_axis: dict[tuple[str, str], list[int]] = defaultdict(list)
    for r in rows:
        key = (r["model_key"], r["_prompt_idx"])
        by_model_prompt[key].append(r["score"]["composite"])
        for axis, val in r["score"]["axes"].items():
            by_model_axis[(r["model_key"], axis)].append(val)

    models = sorted({k[0] for k in by_model_prompt.keys()})
    prompts = sorted({k[1] for k in by_model_prompt.keys()})

    lines = ["# Plan-Quality Rubric Scores\n", f"_Generated {datetime.now().isoformat()}_\n"]

    # Per-prompt table
    lines.append("\n## Composite score (mean / 6) per (model × prompt), N reps\n")
    lines.append("| Prompt | " + " | ".join(models) + " |")
    lines.append("|--------|" + "|".join(["---"] * len(models)) + "|")
    for p in prompts:
        cells = []
        for m in models:
            scores = by_model_prompt.get((m, p), [])
            if scores:
                mean = sum(scores) / len(scores)
                cells.append(f"{mean:.2f}")
            else:
                cells.append("—")
        lines.append(f"| P{p+1:<2} | " + " | ".join(cells) + " |")

    # Per-axis aggregate
    lines.append("\n## Per-axis pass-rate by model (across all prompts × reps)\n")
    axes = list(by_model_axis[(models[0], "routed_correctly")][:0] or [
        "routed_correctly", "interval_correct", "count_correct",
        "t0_preserved", "span_correct", "range_correct"
    ])
    # use canonical order
    axes = ["routed_correctly", "interval_correct", "count_correct",
            "t0_preserved", "span_correct", "range_correct"]
    lines.append("| Model | " + " | ".join(axes) + " |")
    lines.append("|-------|" + "|".join(["---"] * len(axes)) + "|")
    for m in models:
        cells = []
        for ax in axes:
            vals = by_model_axis.get((m, ax), [])
            if vals:
                pass_rate = sum(vals) / len(vals)
                cells.append(f"{pass_rate*100:.0f}%")
            else:
                cells.append("—")
        lines.append(f"| {m} | " + " | ".join(cells) + " |")

    return "\n".join(lines) + "\n"


# ────────────────────────────────────────────────────────────────────────────
# CLI
# ────────────────────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("jsonl", type=Path, help="Path to benchmark JSONL file")
    ap.add_argument("--out-dir", type=Path, default=None, help="Output directory (defaults to JSONL's directory)")
    args = ap.parse_args()

    out_dir = args.out_dir or args.jsonl.parent
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_jsonl = out_dir / f"planning_score_{ts}.jsonl"
    out_md = out_dir / f"plan_scores_{ts}.md"

    rubric_by_idx = {r["idx"]: r for r in RUBRICS}

    scored_rows: list[dict] = []
    with open(args.jsonl, encoding="utf-8") as f, open(out_jsonl, "w", encoding="utf-8") as out_f:
        for line in f:
            row = json.loads(line)
            if row.get("_type") != "planning":
                continue
            idx = row.get("_prompt_idx")
            if idx is None or idx not in rubric_by_idx:
                continue
            score = score_one_plan(rubric_by_idx[idx], row)
            out_row = {
                "_prompt_idx": idx,
                "_rep_index": row.get("_rep_index"),
                "model_key": row.get("model_key"),
                "tool_name": row.get("tool_name"),
                "tool_input": row.get("tool_input"),
                "schedule_count": len(row.get("schedule_json") or []),
                "score": score,
            }
            scored_rows.append(out_row)
            out_f.write(json.dumps(out_row, ensure_ascii=False) + "\n")

    out_md.write_text(render_markdown(scored_rows), encoding="utf-8")

    print(f"Scored {len(scored_rows)} planning rows")
    print(f"  -> {out_jsonl}")
    print(f"  -> {out_md}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
