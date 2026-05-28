"""LLM-as-judge analysis quality scorer.

Reads a benchmark JSONL containing analysis rows and asks Claude Sonnet to
score each (image, backend) finding-set on three axes:

  1. hazard_identification (0-3): how well were real hazards in the image identified
  2. spatial_reasoning     (0-3): how accurately did findings refer to specific objects/positions
  3. false_positive_rate   (0-3): inverse — did findings invent hazards that aren't there

Total score per call: 0-9. Higher = better.

The judge sees:
  - The original image
  - The backend's description + findings + recommendation
  - The original monitoring objective
  - A rubric explaining the three axes

The judge is asked to return STRUCTURED JSON {axes: {...}, reasoning: "..."}.
This avoids parsing free-form prose.

Cost estimate: ~$0.04 per judge call (Sonnet with vision input). For 5 backends
x 20 images x 1 judge round = 100 calls = ~$4.

Usage:
  python scripts/judge_analysis.py results/benchmark_<TS>.jsonl
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

try:
    import anthropic
except ImportError:
    print("anthropic package not installed; pip install anthropic")
    sys.exit(1)


JUDGE_MODEL = "claude-sonnet-4-6"
IMAGES_DIR = Path("D:/- thesis/thesis-iot-monitoring/scripts/benchmark_images")

JUDGE_SYSTEM_PROMPT = """You are evaluating the output of a vision-language model that was asked to analyze an indoor image for safety hazards and notable observations.

You will be shown:
1. The original image
2. The model's three-part finding (description / findings / recommendation)
3. The monitoring objective the model was given

Score the model on THREE independent axes, 0-3 each:

**hazard_identification (0-3)** — how well did the model identify real safety hazards visible in the image?
- 3: identified all major hazards present, with specific named objects
- 2: identified most hazards but missed one notable one OR named hazards in generic terms
- 1: identified only the most obvious hazards (or none, when none were present)
- 0: missed hazards that are visible and meaningful

**spatial_reasoning (0-3)** — did the model accurately describe locations, positions, and spatial relationships of objects?
- 3: precise spatial language (e.g., "between the chair and bookshelf"), correctly grounded
- 2: roughly correct positioning, some vagueness
- 1: generic spatial descriptions with little grounding
- 0: spatial claims that don't match the image

**false_positive_rate (0-3)** — INVERSE: did the model invent hazards or objects that are NOT in the image? Higher = fewer false positives.
- 3: no invented hazards or objects; everything claimed is present
- 2: one minor invention or over-extrapolation
- 1: multiple inventions or significant over-extrapolation
- 0: substantial hallucination (claims things clearly absent)

If the image has NO real hazards visible, a high-quality response should say so (and gets high hazard_identification AND high false_positive_rate). A response that invents hazards in a safe image gets low false_positive_rate.

Return ONLY a JSON object in this exact shape (no other text, no markdown fences):
{
  "axes": {"hazard_identification": <0-3>, "spatial_reasoning": <0-3>, "false_positive_rate": <0-3>},
  "reasoning": "<one-sentence justification per axis, separated by '; '>"
}
"""


def encode_image(path: Path) -> str:
    return base64.standard_b64encode(path.read_bytes()).decode("ascii")


async def judge_one(
    client: anthropic.AsyncAnthropic,
    image_path: Path,
    objective: str,
    description: str,
    findings: str,
    recommendation: str,
) -> dict[str, Any]:
    """Run one judge call. Returns {axes: {...}, reasoning: "..."} or {error: "..."}."""

    finding_block = (
        f"MONITORING OBJECTIVE:\n{objective}\n\n"
        f"MODEL OUTPUT:\n"
        f"  Description: {description}\n"
        f"  Findings: {findings}\n"
        f"  Recommendation: {recommendation}\n"
    )

    image_b64 = encode_image(image_path)

    t0 = time.monotonic()
    try:
        response = await client.messages.create(
            model=JUDGE_MODEL,
            max_tokens=1024,
            system=JUDGE_SYSTEM_PROMPT,
            messages=[
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "image",
                            "source": {
                                "type": "base64",
                                "media_type": "image/jpeg",
                                "data": image_b64,
                            },
                        },
                        {"type": "text", "text": finding_block},
                    ],
                }
            ],
            temperature=0.1,
        )
    except Exception as e:
        return {"error": f"{type(e).__name__}: {e}", "latency_ms": 0}

    elapsed_ms = (time.monotonic() - t0) * 1000
    text = "".join(
        getattr(b, "text", "") for b in response.content if getattr(b, "type", None) == "text"
    )

    # Strip optional markdown fences
    cleaned = text.strip()
    if cleaned.startswith("```"):
        cleaned = cleaned.split("\n", 1)[1].rsplit("```", 1)[0].strip()

    try:
        parsed = json.loads(cleaned)
        return {
            "axes": parsed.get("axes", {}),
            "reasoning": parsed.get("reasoning", ""),
            "raw_text": text,
            "latency_ms": elapsed_ms,
            "input_tokens": response.usage.input_tokens,
            "output_tokens": response.usage.output_tokens,
        }
    except json.JSONDecodeError as e:
        return {
            "error": f"JSON parse failed: {e}",
            "raw_text": text,
            "latency_ms": elapsed_ms,
        }


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("jsonl", type=Path)
    ap.add_argument("--out-dir", type=Path, default=None)
    ap.add_argument(
        "--reps", type=int, default=1,
        help="Reps to score per (image,model). Default 1 (one judge call per "
             "unique pair regardless of how many benchmark reps exist).",
    )
    ap.add_argument(
        "--limit", type=int, default=None,
        help="Cap total judge calls (for cost-control / dry-run). Default no cap.",
    )
    args = ap.parse_args()

    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        print("ERROR: ANTHROPIC_API_KEY environment variable not set", file=sys.stderr)
        return 1

    out_dir = args.out_dir or args.jsonl.parent
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_jsonl = out_dir / f"judge_scores_{ts}.jsonl"
    out_md = out_dir / f"judge_summary_{ts}.md"

    # Build the set of (image, model) pairs to judge.
    # If --reps > 1, we judge that many reps per pair (sampled from earliest).
    # If --reps == 1, we judge only rep_index=0 to keep cost down.
    rows_by_pair: dict[tuple[str, str], list[dict]] = {}
    with open(args.jsonl, encoding="utf-8") as f:
        for line in f:
            row = json.loads(line)
            if row.get("_type") != "analysis":
                continue
            key = (row.get("_image", "?"), row.get("model_key", "?"))
            rows_by_pair.setdefault(key, []).append(row)

    # Pick reps from each pair (rep_index 0 .. args.reps-1 if available)
    judge_tasks: list[tuple[str, str, int, dict]] = []
    for (img, mk), rows in rows_by_pair.items():
        rows_sorted = sorted(rows, key=lambda r: r.get("_rep_index", 0))
        for r in rows_sorted[: args.reps]:
            judge_tasks.append((img, mk, r.get("_rep_index", 0), r))

    if args.limit:
        judge_tasks = judge_tasks[: args.limit]

    print(f"Will judge {len(judge_tasks)} (image, model, rep) cells")
    print(f"  -> {out_jsonl}")

    client = anthropic.AsyncAnthropic(api_key=api_key)
    scored: list[dict] = []

    with open(out_jsonl, "w", encoding="utf-8") as out_f:
        for idx, (img, mk, rep, row) in enumerate(judge_tasks):
            image_path = IMAGES_DIR / img
            if not image_path.exists():
                print(f"  [{idx+1:3}/{len(judge_tasks)}] SKIP missing image {img}")
                continue

            res = row.get("result") or {}
            verdict = await judge_one(
                client,
                image_path,
                row.get("objective", ""),
                res.get("description", "") or "",
                res.get("findings", "") or "",
                res.get("recommendation", "") or "",
            )

            out_row = {
                "_image": img,
                "_model_key": mk,
                "_rep_index": rep,
                "_judged_at": datetime.now().isoformat(),
                "judge_model": JUDGE_MODEL,
                "verdict": verdict,
            }
            scored.append(out_row)
            out_f.write(json.dumps(out_row, ensure_ascii=False) + "\n")
            out_f.flush()

            axes = verdict.get("axes", {})
            composite = sum(axes.values()) if axes else 0
            err = verdict.get("error", "")
            status = f"{composite}/9" if not err else f"ERR: {err[:50]}"
            print(f"  [{idx+1:3}/{len(judge_tasks)}] {mk:<14} {img:<24} {status}")

    # Aggregate
    from collections import defaultdict
    by_model: dict[str, list[int]] = defaultdict(list)
    by_model_axis: dict[tuple[str, str], list[int]] = defaultdict(list)
    for s in scored:
        mk = s["_model_key"]
        axes = s["verdict"].get("axes", {})
        if axes:
            composite = sum(axes.values())
            by_model[mk].append(composite)
            for ax, val in axes.items():
                by_model_axis[(mk, ax)].append(val)

    md_lines = [
        f"# LLM-as-judge Analysis Quality\n",
        f"_Generated {datetime.now().isoformat()} from {args.jsonl.name}_\n",
        f"_Judge model: {JUDGE_MODEL}, N judged = {len(scored)}_\n",
        "\n## Composite score (mean/9) per backend\n",
        "| Backend | N | mean | min | max |",
        "|---------|---:|---:|---:|---:|",
    ]
    for mk in sorted(by_model.keys()):
        vals = by_model[mk]
        md_lines.append(
            f"| {mk} | {len(vals)} | {sum(vals)/len(vals):.2f} | {min(vals)} | {max(vals)} |"
        )

    md_lines.append("\n## Per-axis mean score by backend\n")
    md_lines.append("| Backend | hazard_id | spatial | low_fp |")
    md_lines.append("|---------|---:|---:|---:|")
    for mk in sorted(by_model.keys()):
        cells = []
        for ax in ("hazard_identification", "spatial_reasoning", "false_positive_rate"):
            vals = by_model_axis.get((mk, ax), [])
            cells.append(f"{sum(vals)/len(vals):.2f}" if vals else "—")
        md_lines.append(f"| {mk} | " + " | ".join(cells) + " |")

    out_md.write_text("\n".join(md_lines) + "\n", encoding="utf-8")
    print(f"\nWrote summary: {out_md}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
