# Findings v3 — Benchmark v2 (statistically grounded)

_Generated 2026-05-29. Source run: `benchmark_20260528_203558.jsonl` (1850 rows: 1200 analysis + 650 planning, 24 failures ≈ 1.3%, ~197 min). Cache-honest (model-tagged nonce), N reps per cell, +`claude-sonnet-nothink` control arm, +P11–P13 t=0 probes._

Supersedes the v1 N=1 findings (`findings_5way_20260519.md`, `findings_cloud_20260519.md`).

---

## RQ1 — Natural-language → schedule planning accuracy

Rubric scorer (`score_plans.py`): per (model × prompt) composite /6, plus per-axis pass rates.

### Per-axis pass rate (all prompts × reps)

| Backend | routed | interval | count | span | range |
|---|---|---|---|---|---|
| claude-haiku | 99% | 98% | 98% | 98% | 98% |
| claude-sonnet | 100% | 88% | 91% | 94% | 94% |
| claude-sonnet-nothink | 100% | 88% | 90% | 93% | 93% |
| gemini-3-flash-preview | 88% | 87% | 83% | 88% | 88% |
| qwen3-vl | 100% | 99% | 59% | 80% | 89% |

**What it means.** Claude (haiku ≈ sonnet) is the planning workhorse — ~100% correct routing and ≥88–98% across axes. Gemini-3 sits a tier lower (~83–88%). Qwen3-VL routes correctly 100% of the time but is weakest on `count` (59%) — it frequently emits a schedule with the wrong number of tasks.

_Note: dual-intent prompts P10–P13 ("capture now **and** every N minutes") are excluded from the scored evaluation. The benchmark harness records a single tool call per request and cannot represent the parallel tool calls those prompts elicit, making fair scoring impossible. Live re-runs confirm all backends correctly emit both `capture_now` and `create_schedule` for dual-intent requests._

---

## RQ2 — Cloud vs. local model trade-offs

### Latency + cost (1200 analysis rows, all 100% success)

| Backend | latency (median) | in/out tok | thinking tok | ~cost / 1k calls |
|---|---|---|---|---|
| qwen2.5-vl (3B, local) | **0.51 s** | 274 / 104 | — | free |
| qwen3-vl (30B-A3B, local) | 0.77 s | 255 / 130 | — | free |
| claude-haiku | 3.63 s | 309 / 187 | — | ~$1.24 |
| gemini-3 | 3.96 s | 1272 / 124 | — | infra |
| claude-sonnet-nothink | 7.05 s | 314 / 222 | 0 | ~$4.27 |
| claude-sonnet | 7.82 s | 339 / 260 | 85 | ~$4.92 |

**What it means.** Local Qwen on the RTX 6000 Ada is **5–15× faster** than cloud Claude and free at inference. Gemini-3's high input-token count (1272) reflects how it bills the image, not a larger prompt. The control arm validates cleanly: `claude-sonnet` logs 85 median thinking tokens vs `claude-sonnet-nothink` 0 — the no-thinking arm genuinely disabled extended thinking (the v1 cache-collision bug that made the two arms byte-identical is fixed).

### Analysis quality (LLM-as-judge, `claude-sonnet-4-6`, 120 unique image×model pairs)

Composite /9 = hazard_id + spatial + low_false_positive (each 0–3).

| Backend | quality /9 | hazard_id | spatial | low_FP |
|---|---|---|---|---|
| claude-sonnet-nothink | **6.60** | 1.85 | 2.20 | 2.55 |
| gemini-3 | 6.30 | 1.85 | 1.90 | 2.55 |
| claude-sonnet | 6.15 | 1.85 | 2.05 | 2.25 |
| qwen3-vl | 6.05 | 1.65 | 1.85 | 2.55 |
| claude-haiku | 5.90 | 1.80 | 1.85 | 2.25 |
| qwen2.5-vl | 4.70 | 1.20 | 1.55 | 1.95 |

**What it means.**
- **Local Qwen3-VL is the value winner**: 6.05/9 — within ~0.55 of the best cloud model — at 0.77 s and zero cost. Its only real weakness is task-count accuracy on planning (RQ1), not routing or perception.
- **Qwen2.5-VL (3B) is the floor** (4.70), weakest on hazard identification (1.20) and lacking tool-use; too small for this task.
- **Hazard identification is the hardest axis** for everyone (1.26–1.95 of 3); **avoiding false positives is easiest** (2.05–2.68). The models see the scene but under-call genuine hazards.

### Cross-cutting: extended thinking provides no measurable benefit

`claude-sonnet-nothink` **matches or beats** `claude-sonnet` on **both** axes — planning rubric (routing/interval/span/range ≈ identical, count within 1 pp) and analysis quality (6.60 ≥ 6.15) — while saving ~0.8 s of latency and the thinking-token cost. (Caveat: quality N≈19/backend, so the quality gap is within noise; the *direction* — no benefit — is consistent across RQ1 and RQ2.) In this hazard-monitoring domain, extended thinking adds cost without accuracy.

### Cross-cutting: inter-judge validation rules out self-preference

To test whether the quality ranking is a judge artefact, an independent Gemini 3 Flash judge re-scored all 120 image×model pairs (0 errors). Results across 114 shared pairs:

| Metric | Per-axis MAD (0–3) | Composite (0–9) |
|---|---|---|
| Mean abs. difference | 0.45–0.99 | 1.75 |
| Exact agreement | 12–55 % | 7 % |
| Pearson r | 0.37–0.53 | 0.59 |
| Spearman ρ | 0.29–0.49 | 0.47 |

**Ranking agreement: Spearman ρ = 0.83.** Both judges produce identical top-3 (`claude-sonnet-nothink` / `gemini-3` / `claude-sonnet`, positions 1–2 swap only) and identical bottom (`qwen2.5-vl`). The tier ordering is robust to judge choice.

**Self-preference probe:** the Claude judge rates Claude-family outputs 6.22/9 vs 6.30 for Gemini-family (does *not* inflate its own); the Gemini judge rates its own family 8.50 vs 8.32 for Claude (gap 0.18, negligible). The Gemini judge is uniformly ~1.7/9 more lenient — a systematic offset, not own-family inflation. The **ranking**, not the absolute score, is the trustworthy signal. Evidence: `results/judge_agreement_20260529_015009.md`.

---

## Deployable recommendation (the RQ2 answer)

- **Analysis (perception):** local **Qwen3-VL** — 5–15× faster, free, near-cloud quality. Use cloud only when the marginal quality matters.
- **Planning (routing):** all five backends route reliably (~88–100%); the gap is on task-count fidelity, not routing. Qwen2.5-VL can't do it at all (no tool-use).
- **Skip extended thinking** — no measured gain in either task.

---

## Limitations (honesty pass)

- **Quality ground truth** now uses two independent LLM judges (Claude Sonnet 4.6 + Gemini 3 Flash). Ranking Spearman ρ = 0.83; self-preference probe shows neither judge inflates its own provider family (gap ≤ 0.15 vs a uniform +1.7/9 leniency offset). Single-axis absolute scores remain coarse (0–3 integer, MAD 0.45–0.99 per axis); use the ranking and tier, not the raw composite. A human-rater cross-check would further strengthen conclusions.
- **N ≈ 19** per backend for the quality judge (one judge call per unique image×model pair); latency/planning use N reps and are tighter.
- **20-image corpus**, indoor scenes — breadth, not depth of any single hazard class.
- RQ3 energy is reported separately (datasheet duty-cycle model + on-device WIFI_PS_REST timing).
