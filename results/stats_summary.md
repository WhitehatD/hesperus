# Inferential statistics (RQ2)

_Generated 2026-06-05T02:29:59 by stats_analysis.py (seed=42, 10000 bootstrap resamples)._

Sources: `judge_scores_20260529_000539.jsonl` (quality), `planning_score_20260529_000027.jsonl` (planning).

## RQ2 quality: bootstrap 95% CI on composite /9 (N=20 per backend)

| Backend | Mean /9 | 95% CI |
|---|---|---|
| claude-sonnet-nothink | 6.60 | [5.75, 7.20] |
| gemini-3 | 6.30 | [5.50, 6.80] |
| claude-sonnet | 6.15 | [5.40, 6.70] |
| qwen3-vl | 6.05 | [5.25, 6.65] |
| claude-haiku | 5.90 | [5.10, 6.45] |
| qwen2.5-vl | 4.70 | [4.00, 5.30] |

## RQ2 quality: omnibus (Kruskal-Wallis)

- All six backends: H = 32.82, p = 4.079e-06 (significant; driven by the 3B floor model).
- Top five (excl. Qwen2.5-VL-3B): H = 7.58, p = 0.108 (no significant difference).

## RQ2 quality: all ten top-five pairwise exact Wilcoxon + Holm (over the 10-pair family)

| Comparison (a vs b) | n | mean diff | p (exact) | p (Holm) | sig |
|---|---|---|---|---|---|
| claude-haiku vs claude-sonnet | 20 | -0.25 | 0.250 | 1.000 | ns |
| claude-haiku vs claude-sonnet-nothink | 20 | -0.70 | 0.005 | 0.049 | * |
| claude-haiku vs gemini-3 | 20 | -0.40 | 0.054 | 0.376 | ns |
| claude-haiku vs qwen3-vl | 20 | -0.15 | 0.577 | 1.000 | ns |
| claude-sonnet vs claude-sonnet-nothink | 20 | -0.45 | 0.042 | 0.336 | ns |
| claude-sonnet vs gemini-3 | 20 | -0.15 | 0.492 | 1.000 | ns |
| claude-sonnet vs qwen3-vl | 20 | +0.10 | 0.695 | 1.000 | ns |
| claude-sonnet-nothink vs gemini-3 | 20 | +0.30 | 0.129 | 0.773 | ns |
| claude-sonnet-nothink vs qwen3-vl | 20 | +0.55 | 0.037 | 0.334 | ns |
| gemini-3 vs qwen3-vl | 20 | +0.25 | 0.219 | 1.000 | ns |
- Minimum top-five p(Holm) = 0.049 (at least one significant).

## RQ2 quality: 3B floor model vs each of the top five (Holm over the 5-pair family)

| Comparison (a vs b) | n | mean diff | p (exact) | p (Holm) | sig |
|---|---|---|---|---|---|
| claude-haiku vs qwen2.5-vl | 20 | +1.20 | 0.001 | 0.001 | * |
| claude-sonnet vs qwen2.5-vl | 20 | +1.45 | 0.000 | 0.000 | * |
| claude-sonnet-nothink vs qwen2.5-vl | 20 | +1.90 | 0.000 | 0.000 | * |
| gemini-3 vs qwen2.5-vl | 20 | +1.60 | 0.000 | 0.000 | * |
| qwen3-vl vs qwen2.5-vl | 20 | +1.35 | 0.000 | 0.001 | * |

## RQ2 quality: inter-judge self-preference + rank correlation (recomputed from both canonical judge files)

- Spearman rank correlation between judges: **0.83**
- Claude judge -> Claude-family **6.22** vs Gemini-family **6.30** (rates the other family +0.08).
- Gemini judge -> Gemini-family **8.50** vs Claude-family **8.32** (own-family lean +0.18).
- Neither judge inflates its own family. Supersedes the stale 6.54 / 6.63 / 8.47 in judge_agreement_20260529_015009.md.
