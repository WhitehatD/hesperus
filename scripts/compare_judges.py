"""Inter-judge agreement for the RQ2 analysis-quality evaluation.

The quality scores in findings_v3.md come from a single LLM judge (Claude
Sonnet 4.6), which raises a self-preference concern: does the judge favour
Claude-family backends? This script quantifies that by comparing two
INDEPENDENT judges (e.g. Claude Sonnet vs. Gemini 3 Flash) over the same
image x model pairs.

It reports, with NO third-party dependencies (stdlib only):
  - per-axis and composite mean-absolute-difference + exact-agreement rate
  - per-axis and composite Pearson + Spearman correlation between judges
  - per-backend mean composite from each judge, side by side, with the gap
  - the BACKEND RANKING produced by each judge + Spearman rank correlation
    (the headline: do the two judges agree on who is best?)
  - a self-preference probe: does each judge rate its OWN family higher than
    the other judge does?

Usage:
  python scripts/compare_judges.py JUDGE_A.jsonl JUDGE_B.jsonl [--out report.md]
  # e.g. claude judge vs gemini judge:
  python scripts/compare_judges.py \\
      results/judge_scores_20260529_000539.jsonl \\
      results/judge_scores_gemini_YYYYMMDD_HHMMSS.jsonl
"""
from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from datetime import datetime
from pathlib import Path

AXES = ["hazard_identification", "spatial_reasoning", "false_positive_rate"]


def load_scores(path: Path) -> tuple[str, dict[tuple[str, str], dict]]:
    """Load a judge_scores JSONL -> (judge_model, {(image, model_key): {axes, composite}})."""
    out: dict[tuple[str, str], dict] = {}
    judge_model = "unknown"
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            judge_model = row.get("judge_model", judge_model)
            verdict = row.get("verdict") or {}
            axes = verdict.get("axes") or {}
            # Skip rows that errored (no axes) — they cannot be compared.
            if not all(a in axes for a in AXES):
                continue
            key = (row.get("_image", ""), row.get("_model_key", ""))
            composite = sum(float(axes[a]) for a in AXES)
            out[key] = {"axes": {a: float(axes[a]) for a in AXES}, "composite": composite}
    return judge_model, out


def pearson(xs: list[float], ys: list[float]) -> float:
    n = len(xs)
    if n < 2:
        return float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    dx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    dy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if dx == 0 or dy == 0:
        return float("nan")
    return num / (dx * dy)


def _ranks(vals: list[float]) -> list[float]:
    """Average ranks (ties share the mean rank)."""
    order = sorted(range(len(vals)), key=lambda i: vals[i])
    ranks = [0.0] * len(vals)
    i = 0
    while i < len(vals):
        j = i
        while j + 1 < len(vals) and vals[order[j + 1]] == vals[order[i]]:
            j += 1
        avg = (i + j) / 2.0 + 1.0  # 1-based average rank
        for k in range(i, j + 1):
            ranks[order[k]] = avg
        i = j + 1
    return ranks


def spearman(xs: list[float], ys: list[float]) -> float:
    if len(xs) < 2:
        return float("nan")
    return pearson(_ranks(xs), _ranks(ys))


def fmt(x: float, p: int = 2) -> str:
    return "n/a" if (x != x) else f"{x:.{p}f}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("judge_a", type=Path, help="First judge_scores JSONL (e.g. Claude judge)")
    ap.add_argument("judge_b", type=Path, help="Second judge_scores JSONL (e.g. Gemini judge)")
    ap.add_argument("--out", type=Path, default=None, help="Markdown output path (default: results/judge_agreement_<ts>.md)")
    args = ap.parse_args()

    for p in (args.judge_a, args.judge_b):
        if not p.exists():
            print(f"ERROR: file not found: {p}")
            return 1

    name_a, a = load_scores(args.judge_a)
    name_b, b = load_scores(args.judge_b)

    shared = sorted(set(a) & set(b))
    if not shared:
        print("ERROR: no overlapping (image, model_key) pairs between the two judges.")
        return 1

    lines: list[str] = []
    lines.append(f"# Inter-judge agreement: {name_a} vs. {name_b}\n")
    lines.append(f"_Generated {datetime.now().isoformat(timespec='seconds')}. "
                 f"{len(shared)} shared image x model pairs "
                 f"(judge A: {len(a)}, judge B: {len(b)})._\n")

    # ── Per-axis + composite agreement ────────────────────────────────────────
    lines.append("## Per-axis agreement\n")
    lines.append("| Metric | Mean abs. diff (0-3) | Exact agreement | Pearson r | Spearman rho |")
    lines.append("|---|---|---|---|---|")
    for axis in AXES + ["composite"]:
        if axis == "composite":
            xs = [a[k]["composite"] for k in shared]
            ys = [b[k]["composite"] for k in shared]
        else:
            xs = [a[k]["axes"][axis] for k in shared]
            ys = [b[k]["axes"][axis] for k in shared]
        mad = sum(abs(x - y) for x, y in zip(xs, ys)) / len(xs)
        exact = sum(1 for x, y in zip(xs, ys) if x == y) / len(xs)
        label = axis.replace("_", " ")
        bold = "**" if axis == "composite" else ""
        scale = "(0-9)" if axis == "composite" else "(0-3)"
        lines.append(
            f"| {bold}{label} {scale}{bold} | {fmt(mad)} | {exact*100:.0f}% | "
            f"{fmt(pearson(xs, ys))} | {fmt(spearman(xs, ys))} |"
        )
    lines.append("")

    # ── Per-backend mean composite from each judge ────────────────────────────
    lines.append("## Per-backend mean composite (each judge)\n")
    by_model_a: dict[str, list[float]] = defaultdict(list)
    by_model_b: dict[str, list[float]] = defaultdict(list)
    for (img, mk) in shared:
        by_model_a[mk].append(a[(img, mk)]["composite"])
        by_model_b[mk].append(b[(img, mk)]["composite"])
    models = sorted(by_model_a, key=lambda m: sum(by_model_a[m]) / len(by_model_a[m]), reverse=True)
    lines.append(f"| Backend | {name_a} /9 | {name_b} /9 | Gap |")
    lines.append("|---|---|---|---|")
    mean_a, mean_b = {}, {}
    for m in models:
        ma = sum(by_model_a[m]) / len(by_model_a[m])
        mb = sum(by_model_b[m]) / len(by_model_b[m])
        mean_a[m], mean_b[m] = ma, mb
        lines.append(f"| {m} | {fmt(ma)} | {fmt(mb)} | {fmt(ma - mb, 2)} |")
    lines.append("")

    # ── Ranking agreement (the headline) ──────────────────────────────────────
    rank_a = sorted(models, key=lambda m: mean_a[m], reverse=True)
    rank_b = sorted(models, key=lambda m: mean_b[m], reverse=True)
    common = [m for m in mean_a if m in mean_b]
    rho = spearman([mean_a[m] for m in common], [mean_b[m] for m in common])
    lines.append("## Backend ranking agreement\n")
    lines.append(f"- **{name_a} ranking:** {' > '.join(rank_a)}")
    lines.append(f"- **{name_b} ranking:** {' > '.join(rank_b)}")
    lines.append(f"- **Spearman rank correlation between judges: {fmt(rho)}** "
                 f"({'identical ranking' if rank_a == rank_b else 'rankings differ'}).")
    lines.append("")

    # ── Self-preference probe ─────────────────────────────────────────────────
    # Does judge A rate its own family higher (vs judge B) than it rates others?
    lines.append("## Self-preference probe\n")
    lines.append("Mean composite each judge assigns, grouped by whether the JUDGED backend "
                 "shares the judge's provider family. A judge inflating its own family would "
                 "show a positive (own - other) gap that the independent judge does not.\n")

    def family(model_key: str) -> str:
        mk = model_key.lower()
        if "claude" in mk:
            return "claude"
        if "gemini" in mk:
            return "gemini"
        return "other"

    def judge_family(jname: str) -> str:
        return family(jname)

    fam_a, fam_b = judge_family(name_a), judge_family(name_b)
    lines.append(f"| Judged family | {name_a} mean /9 | {name_b} mean /9 |")
    lines.append("|---|---|---|")
    for fam in ["claude", "gemini", "other"]:
        ks = [k for k in shared if family(k[1]) == fam]
        if not ks:
            continue
        avg_a = sum(a[k]["composite"] for k in ks) / len(ks)
        avg_b = sum(b[k]["composite"] for k in ks) / len(ks)
        tag = []
        if fam == fam_a:
            tag.append(f"{name_a}'s own")
        if fam == fam_b:
            tag.append(f"{name_b}'s own")
        suffix = f" ({', '.join(tag)})" if tag else ""
        lines.append(f"| {fam}{suffix} | {fmt(avg_a)} | {fmt(avg_b)} |")
    lines.append("")
    lines.append("> Interpretation: high composite correlation + matching ranking across two "
                 "independent-provider judges is evidence the quality ranking is not an artefact "
                 "of single-judge self-preference. Inspect the self-preference table for any "
                 "family that one judge inflates relative to the other.")
    lines.append("")

    report = "\n".join(lines)
    out_path = args.out or (Path(__file__).resolve().parent.parent / "results" /
                            f"judge_agreement_{datetime.now():%Y%m%d_%H%M%S}.md")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(report, encoding="utf-8")

    # Console summary
    comp_xs = [a[k]["composite"] for k in shared]
    comp_ys = [b[k]["composite"] for k in shared]
    print(f"Shared pairs: {len(shared)}")
    print(f"Composite Pearson r : {fmt(pearson(comp_xs, comp_ys))}")
    print(f"Composite Spearman  : {fmt(spearman(comp_xs, comp_ys))}")
    print(f"Backend rank rho    : {fmt(rho)} ({'same' if rank_a == rank_b else 'differ'})")
    print(f"-> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
