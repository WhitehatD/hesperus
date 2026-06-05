#!/usr/bin/env python3
"""Statistical analysis for the thesis evaluation (RQ2).

Computes the inferential statistics that back the claims in the paper:

  RQ2 (analysis quality, LLM-as-judge composite /9, N=20 images per backend):
    - Bootstrap 95% confidence intervals on the per-backend mean composite.
    - Kruskal-Wallis omnibus across all six backends, and across the top five
      (i.e. excluding the 3B floor model) to test whether the leading models
      are separable at all.
    - Pairwise exact Wilcoxon signed-rank tests (paired by image) with
      Holm-Bonferroni correction for the multiple-comparison family.

Reads only the committed canonical result files; emits a timestamped markdown
summary next to them. Pure stdlib + numpy + scipy, deterministic (fixed seed).

Usage:
    python results/stats_analysis.py
"""

from __future__ import annotations

import json
import datetime as _dt
from pathlib import Path

import numpy as np
from scipy import stats

RESULTS_DIR = Path(__file__).resolve().parent
JUDGE_FILE = RESULTS_DIR / "judge_scores_20260529_000539.jsonl"      # canonical Claude judge
JUDGE_FILE_B = RESULTS_DIR / "judge_scores_20260529_013824.jsonl"    # canonical Gemini judge
PLANNING_FILE = RESULTS_DIR / "planning_score_20260529_000027.jsonl"  # canonical rubric scores

CLAUDE_FAMILY = ("claude-haiku", "claude-sonnet", "claude-sonnet-nothink")
GEMINI_FAMILY = ("gemini-3",)

QUALITY_AXES = ("hazard_identification", "spatial_reasoning", "false_positive_rate")
BOOTSTRAP_RESAMPLES = 10_000
SEED = 42


def _read_jsonl(path: Path) -> list[dict]:
    with path.open(encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]


# --------------------------------------------------------------------------- #
# RQ2: analysis quality
# --------------------------------------------------------------------------- #
def load_quality() -> dict[str, dict]:
    """Return {model_key: {"by_image": {image: composite}, "scores": [...]}}."""
    rows = _read_jsonl(JUDGE_FILE)
    per_model: dict[str, dict] = {}
    for row in rows:
        axes = row["verdict"].get("axes", {})
        composite = sum(v for k, v in axes.items()
                        if k in QUALITY_AXES and isinstance(v, (int, float)))
        model = row["_model_key"]
        per_model.setdefault(model, {"by_image": {}})["by_image"][row["_image"]] = composite
    for model, data in per_model.items():
        data["scores"] = list(data["by_image"].values())
    return per_model


def bootstrap_ci(scores: list[int], rng: np.random.Generator,
                 resamples: int = BOOTSTRAP_RESAMPLES) -> tuple[float, float, float]:
    arr = np.asarray(scores, dtype=float)
    means = np.sort([rng.choice(arr, size=arr.size, replace=True).mean()
                     for _ in range(resamples)])
    lo = means[int(0.025 * resamples)]
    hi = means[int(0.975 * resamples)]
    return float(arr.mean()), float(lo), float(hi)


def holm_bonferroni(pvals: list[float]) -> list[float]:
    order = sorted(range(len(pvals)), key=lambda i: pvals[i])
    m = len(pvals)
    adjusted = [0.0] * m
    running_max = 0.0
    for rank, idx in enumerate(order):
        val = min(1.0, pvals[idx] * (m - rank))
        running_max = max(running_max, val)  # enforce monotonicity
        adjusted[idx] = running_max
    return adjusted


def paired_wilcoxon(a_scores: dict[str, int], b_scores: dict[str, int]) -> tuple[int, float]:
    images = sorted(set(a_scores) & set(b_scores))
    a = np.array([a_scores[i] for i in images], dtype=float)
    b = np.array([b_scores[i] for i in images], dtype=float)
    if np.all(a == b):
        return len(images), 1.0
    try:
        p = stats.wilcoxon(a, b, zero_method="wilcox", correction=False,
                           method="exact").pvalue
    except ValueError:
        p = stats.wilcoxon(a, b, zero_method="wilcox").pvalue
    return len(images), float(p)


def composite_by_image_model(path: Path) -> dict[tuple[str, str], int]:
    """{(image, model): composite} for one judge file."""
    out: dict[tuple[str, str], int] = {}
    for row in _read_jsonl(path):
        axes = row["verdict"].get("axes", {})
        out[(row["_image"], row["_model_key"])] = sum(
            v for k, v in axes.items() if k in QUALITY_AXES and isinstance(v, (int, float)))
    return out


def family_mean_for_judge(pim: dict[tuple[str, str], int], family: tuple[str, ...]) -> float:
    vals = [s for (img, m), s in pim.items() if m in family]
    return float(np.mean(vals)) if vals else float("nan")


def inter_judge() -> dict:
    """Self-preference (per family, per judge) + Spearman rank correlation."""
    a = composite_by_image_model(JUDGE_FILE)     # Claude judge
    b = composite_by_image_model(JUDGE_FILE_B)   # Gemini judge

    def per_model_mean(pim):
        agg: dict[str, list[int]] = {}
        for (img, m), s in pim.items():
            agg.setdefault(m, []).append(s)
        return {m: float(np.mean(v)) for m, v in agg.items()}

    a_means, b_means = per_model_mean(a), per_model_mean(b)
    models = sorted(set(a_means) & set(b_means))
    rho, _ = stats.spearmanr([a_means[m] for m in models], [b_means[m] for m in models])

    return {
        "claude_judge": {"claude_family": family_mean_for_judge(a, CLAUDE_FAMILY),
                         "gemini_family": family_mean_for_judge(a, GEMINI_FAMILY)},
        "gemini_judge": {"gemini_family": family_mean_for_judge(b, GEMINI_FAMILY),
                         "claude_family": family_mean_for_judge(b, CLAUDE_FAMILY)},
        "spearman_rho": float(rho),
    }


# --------------------------------------------------------------------------- #
# Report
# --------------------------------------------------------------------------- #
def main() -> None:
    rng = np.random.default_rng(SEED)
    quality = load_quality()

    ranked = sorted(quality, key=lambda m: -np.mean(quality[m]["scores"]))

    ci_lines = []
    for model in ranked:
        mean, lo, hi = bootstrap_ci(quality[model]["scores"], rng)
        ci_lines.append((model, mean, lo, hi))

    # Inter-judge self-preference + rank correlation, recomputed from BOTH
    # canonical judge files (supersedes the stale 6.54 / 6.63 / 8.47 figures in
    # judge_agreement_20260529_015009.md).
    ij = inter_judge()
    claude_family = ij["claude_judge"]["claude_family"]
    gemini_family = ij["claude_judge"]["gemini_family"]

    # Omnibus tests
    all_groups = [quality[m]["scores"] for m in quality]
    h_all, p_all = stats.kruskal(*all_groups)
    top5 = [m for m in quality if m != "qwen2.5-vl"]
    h_top5, p_top5 = stats.kruskal(*[quality[m]["scores"] for m in top5])

    # Pairwise exact Wilcoxon. Test ALL ten top-five pairs (not a hand-picked
    # subset) so "no two of the five differ" is an honest claim; Holm-correct
    # within the ten-pair family. The 3B floor model is compared against each of
    # the five separately (its own five-pair family).
    import itertools
    top5_pairs = list(itertools.combinations(top5, 2))        # 10 pairs
    floor_pairs = [(m, "qwen2.5-vl") for m in top5]           # 5 pairs

    def _run(pair_list):
        rows = []
        for a, b in pair_list:
            n, p = paired_wilcoxon(quality[a]["by_image"], quality[b]["by_image"])
            diff = np.mean(quality[a]["scores"]) - np.mean(quality[b]["scores"])
            rows.append((a, b, n, p, float(diff)))
        return rows, holm_bonferroni([r[3] for r in rows])

    raw, holm = _run(top5_pairs)
    floor_raw, floor_holm = _run(floor_pairs)

    # --- emit markdown (stable filename; provenance timestamp is in the header) ---
    out = RESULTS_DIR / "stats_summary.md"
    L = []
    L.append(f"# Inferential statistics (RQ2)\n")
    L.append(f"_Generated {_dt.datetime.now().isoformat(timespec='seconds')} "
             f"by stats_analysis.py (seed={SEED}, {BOOTSTRAP_RESAMPLES} bootstrap resamples)._\n")
    L.append(f"Sources: `{JUDGE_FILE.name}` (quality), `{PLANNING_FILE.name}` (planning).\n")

    L.append("## RQ2 quality: bootstrap 95% CI on composite /9 (N=20 per backend)\n")
    L.append("| Backend | Mean /9 | 95% CI |")
    L.append("|---|---|---|")
    for model, mean, lo, hi in ci_lines:
        L.append(f"| {model} | {mean:.2f} | [{lo:.2f}, {hi:.2f}] |")
    L.append("")

    L.append("## RQ2 quality: omnibus (Kruskal-Wallis)\n")
    L.append(f"- All six backends: H = {h_all:.2f}, p = {p_all:.4g} "
             f"(significant; driven by the 3B floor model).")
    L.append(f"- Top five (excl. Qwen2.5-VL-3B): H = {h_top5:.2f}, p = {p_top5:.3f} "
             f"({'no significant difference' if p_top5 >= 0.05 else 'significant'}).")
    L.append("")

    L.append("## RQ2 quality: all ten top-five pairwise exact Wilcoxon + Holm (over the 10-pair family)\n")
    L.append("| Comparison (a vs b) | n | mean diff | p (exact) | p (Holm) | sig |")
    L.append("|---|---|---|---|---|---|")
    for (a, b, n, p, diff), p_holm in zip(raw, holm):
        sig = "*" if p_holm < 0.05 else "ns"
        L.append(f"| {a} vs {b} | {n} | {diff:+.2f} | {p:.3f} | {p_holm:.3f} | {sig} |")
    L.append(f"- Minimum top-five p(Holm) = {min(holm):.3f} "
             f"({'all non-significant' if min(holm) >= 0.05 else 'at least one significant'}).")
    L.append("")
    L.append("## RQ2 quality: 3B floor model vs each of the top five (Holm over the 5-pair family)\n")
    L.append("| Comparison (a vs b) | n | mean diff | p (exact) | p (Holm) | sig |")
    L.append("|---|---|---|---|---|---|")
    for (a, b, n, p, diff), p_holm in zip(floor_raw, floor_holm):
        sig = "*" if p_holm < 0.05 else "ns"
        L.append(f"| {a} vs {b} | {n} | {diff:+.2f} | {p:.3f} | {p_holm:.3f} | {sig} |")
    L.append("")

    gj_own = ij["gemini_judge"]["gemini_family"]
    gj_other = ij["gemini_judge"]["claude_family"]
    L.append("## RQ2 quality: inter-judge self-preference + rank correlation "
             "(recomputed from both canonical judge files)\n")
    L.append(f"- Spearman rank correlation between judges: **{ij['spearman_rho']:.2f}**")
    L.append(f"- Claude judge -> Claude-family **{claude_family:.2f}** vs Gemini-family "
             f"**{gemini_family:.2f}** (rates the other family {gemini_family - claude_family:+.2f}).")
    L.append(f"- Gemini judge -> Gemini-family **{gj_own:.2f}** vs Claude-family "
             f"**{gj_other:.2f}** (own-family lean {gj_own - gj_other:+.2f}).")
    L.append("- Neither judge inflates its own family. "
             "Supersedes the stale 6.54 / 6.63 / 8.47 in judge_agreement_20260529_015009.md.")
    L.append("")

    out.write_text("\n".join(L), encoding="utf-8")

    # also echo to stdout
    print("\n".join(L))
    print(f"\nWrote {out}")


if __name__ == "__main__":
    main()
