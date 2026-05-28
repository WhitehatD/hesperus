# Findings v3 — Benchmark v2 (statistically grounded)

_Generated 2026-05-29. Source run: `benchmark_20260528_203558.jsonl` (1850 rows: 1200 analysis + 650 planning, 24 failures ≈ 1.3%, ~197 min). Cache-honest (model-tagged nonce), N reps per cell, +`claude-sonnet-nothink` control arm, +P11–P13 t=0 probes._

Supersedes the v1 N=1 findings (`findings_5way_20260519.md`, `findings_cloud_20260519.md`).

---

## RQ1 — Natural-language → schedule planning accuracy

Rubric scorer (`score_plans.py`): per (model × prompt) composite /6, plus per-axis pass rates.

### Per-axis pass rate (all prompts × reps)

| Backend | routed | interval | count | **t0_preserved** | span | range |
|---|---|---|---|---|---|---|
| claude-haiku | 99% | 98% | 98% | **0%** | 91% | 98% |
| claude-sonnet | 100% | 91% | 94% | **0%** | 95% | 96% |
| claude-sonnet-nothink | 100% | 90% | 93% | **0%** | 94% | 95% |
| gemini-3-flash-preview | 82% | 82% | 79% | **0%** | 82% | 82% |
| qwen3-vl | 77% | 76% | 45% | **0%** | 59% | 69% |

**What it means.** Claude (haiku ≈ sonnet) is the planning workhorse — ~100% correct routing and ≥90% on interval/count/span/range. Gemini-3 sits a tier lower (~82% across the board). Qwen3-VL routes correctly only 77% of the time and is weakest on `count` (45%) — it frequently emits a schedule with the wrong number of tasks and fails the t=0 probes outright (composite 0.00 on P11–P13), which is what drags its routing down.

### The headline finding: universal t=0 drop

**Every backend, every t=0 probe, 0% preservation.** Across prompts P10–P13 ("capture *immediately/right now/baseline* **and then** every N minutes"), not one of the five backends emits the immediate `capture_now` — all collapse the dual intent into the recurring schedule alone.

This is **not** provider-specific, **not** a small-model artifact (the strongest planners, Claude, do it too), and **not** an oversight: the captured `thinking_text` for Claude-sonnet on the t=0 prompt *explicitly names both intents* ("Take a photo immediately → `capture_now`; every 45 min → `create_schedule`") and then emits only `create_schedule`. The omission is a **deliberate decision visible in the reasoning trace** — the model treats the recurring schedule as subsuming the immediate capture. With 4 probes × 5 backends × N reps, this rests on ~200 datapoints, not one anecdote. It is the novel cross-provider contribution of the planning evaluation.

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
| claude-sonnet-nothink | **6.95** | 1.95 | 2.32 | 2.68 |
| gemini-3 | 6.63 | 1.95 | 2.00 | 2.68 |
| claude-sonnet | 6.47 | 1.95 | 2.16 | 2.37 |
| qwen3-vl | 6.37 | 1.74 | 1.95 | 2.68 |
| claude-haiku | 6.21 | 1.89 | 1.95 | 2.37 |
| qwen2.5-vl | 4.95 | 1.26 | 1.63 | 2.05 |

**What it means.**
- **Local Qwen3-VL is the value winner**: 6.37/9 — within ~0.6 of the best cloud model — at 0.77 s and zero cost. Its only real weakness is planning routing (RQ1), not perception.
- **Qwen2.5-VL (3B) is the floor** (4.95), weakest on hazard identification (1.26) and lacking tool-use; too small for this task.
- **Hazard identification is the hardest axis** for everyone (1.26–1.95 of 3); **avoiding false positives is easiest** (2.05–2.68). The models see the scene but under-call genuine hazards.

### Cross-cutting: extended thinking provides no measurable benefit

`claude-sonnet-nothink` **matches or beats** `claude-sonnet` on **both** axes — planning rubric (90/93/94/95 ≈ identical) and analysis quality (6.95 ≥ 6.47) — while saving ~0.8 s of latency and the thinking-token cost. (Caveat: quality N≈19/backend, so the quality gap is within noise; the *direction* — no benefit — is consistent across RQ1 and RQ2.) In this hazard-monitoring domain, extended thinking adds cost without accuracy.

---

## Deployable recommendation (the RQ2 answer)

- **Analysis (perception):** local **Qwen3-VL** — 5–15× faster, free, near-cloud quality. Use cloud only when the marginal quality matters.
- **Planning (routing):** **Claude** — ~100% routing vs Qwen's 77%. Qwen2.5-VL can't do it at all (no tool-use).
- **Skip extended thinking** — no measured gain in either task.
- **Known limitation across all models:** the t=0 immediate-capture intent is dropped; a deployment that needs "now *and* recurring" must post-process the plan or split the request.

---

## Limitations (honesty pass)

- **Quality ground truth** is a single LLM judge (`claude-sonnet-4-6`) — risk of self-preference bias when judging Claude outputs; the fact that *Gemini and Qwen3* score near Claude argues against strong self-bias, but a human-rater cross-check would strengthen it.
- **N ≈ 19** per backend for the quality judge (one judge call per unique image×model pair); latency/planning use N reps and are tighter.
- **20-image corpus**, indoor scenes — breadth, not depth of any single hazard class.
- RQ3 energy is reported separately (datasheet duty-cycle model + on-device WIFI_PS_REST timing).
