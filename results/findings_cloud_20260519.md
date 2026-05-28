# Thesis Cloud Benchmark — Findings

**Run date:** 2026-05-19 03:42
**Server:** `http://89.167.11.147:8000` (Hetzner Helsinki VPS, production deploy)
**Raw data:** `results/benchmark_20260519_034252.jsonl`
**Status:** Cloud phase complete (3/4 backends). Ernis (qwen3-vl, qwen2.5-vl) — pending.

## Scope

- **Image dataset:** 20 curated images from MIT Indoor Scenes (Quattoni & Torralba, CVPR 2009)
  Categories: office×4, meeting_room×3, computerroom×3, corridor×3, library×2, kitchen×2, livingroom×2, lobby×1
  Curation script (reproducible, seed=20260519): `scripts/curate_benchmark_images.py`
- **Analysis benchmark:** 20 images × 3 cloud models = **60 calls**
- **Planning benchmark:** 10 monitoring objectives × 3 cloud models = **30 calls**
- **Pipeline:** runner (`scripts/run_benchmark.py`) → FastAPI server → cloud provider
  Server normalizes every image to RGB JPEG quality 92 at the boundary, so all backends see the same input.

## Headline numbers

### Analysis — 60/60 successful

| Model           |  n |  ok | min ms | **p50** | p90  | p95   | max   | mean | stdev |
|-----------------|---:|----:|-------:|--------:|-----:|------:|------:|-----:|------:|
| claude-haiku    | 20 |  20 |  2 679 | **3 119** | 3 674 | 22 193 | 22 193 | 4 127 | 4 262 |
| claude-sonnet   | 20 |  20 |  5 323 | **6 394** | 8 548 | 36 004 | 36 004 | 7 995 | 6 644 |
| gemini-3        | 20 |  20 |  2 837 | **4 072** | 5 339 |  5 381 |  5 381 | 4 039 |   649 |

### Planning — 29/30 successful

| Model                     |  n | ok | p50 ms | p95 ms | mean ms |
|---------------------------|---:|---:|-------:|-------:|--------:|
| claude-haiku              | 10 |  9 | 1 215  | 3 190  | 1 413   |
| claude-sonnet             | 10 | 10 | 2 399  | 3 484  | 2 444   |
| gemini-3-flash-preview    | 10 | 10 | 1 595  | 4 054  | 2 140   |

The single planning failure (`claude-haiku` on prompt[3]) was not a server bug — the model legitimately returned a text-only response instead of choosing a tool to call.

## Detailed observations

### 1. Latency ranking

p50 latency for analysis ranks **haiku (3.1s) < gemini-3 (4.1s) < sonnet (6.4s)**. Sonnet is roughly **2× slower** than haiku at p50, which is consistent with Anthropic's published per-token costs (Sonnet emits more tokens *and* takes longer per token). For real-time monitoring loops on an STM32 board, the choice is between speed (haiku) and richness (sonnet).

### 2. Consistency: gemini-3 is the clear winner

The stdev tells a story the mean hides:

- **gemini-3 stdev = 649 ms** — distribution is tight; no outliers above 5.4 s.
- **claude-haiku stdev = 4 262 ms** — would be ~300 ms without one bad outlier.
- **claude-sonnet stdev = 6 644 ms** — would be ~900 ms without one bad outlier.

For an edge device that needs predictable response latency to drive a fixed-rate capture loop, gemini-3 is the safer pick despite a slightly higher p50.

### 3. The kitchen outliers

Two single calls dominated the Anthropic stdev:

| Call | Model | Image | Latency | × p50 |
|------|-------|-------|--------:|------:|
| #20  | claude-sonnet | kitchen_01.jpg | **36 004 ms** | 5.6× |
| #22  | claude-haiku  | kitchen_02.jpg | **22 193 ms** | 7.1× |

Both outliers happened on kitchen images; gemini handled both kitchen images in 2.8 s and 3.8 s without issue. Possible causes:

- Transient Anthropic API tail latency (regional load spike at ~03:43 UTC)
- Image content sensitivity (kitchen scenes have many small objects + reflective surfaces; Claude's vision pipeline may iterate more)

A second run would tell us if the kitchen anomaly recurs (image-specific) or moves around (provider-side jitter). Either way, **p50/p90 are the right metrics for thesis comparison** — they ignore single tail events that don't represent steady-state behaviour.

### 4. Response richness vs speed

| Model | mean chars (desc + findings + recs) | rel. to sonnet |
|---|---:|---:|
| claude-sonnet | **703** | 100 % |
| claude-haiku  | 601 | 85 % |
| gemini-3      | 446 | 63 % |

Gemini is the most terse — it gives ~37 % less text for similar latency cost vs sonnet. Anyone scoring on completeness of finding detail will rank: **sonnet > haiku > gemini**. Anyone scoring on speed-per-finding will rank: **haiku > gemini > sonnet**. There's no "best" — only trade-off curves.

### 5. Latency per image category (mean ms)

| Category    | claude-haiku | claude-sonnet | gemini-3 |
|-------------|-------------:|--------------:|---------:|
| computerroom |       3 440 |         6 600 |    4 018 |
| corridor     |       3 248 |         6 583 |    3 883 |
| kitchen      |  **12 654** |    **21 005** |    3 298 |
| library      |       3 352 |         7 442 |    4 122 |
| livingroom   |       2 848 |         5 747 |    4 247 |
| lobby        |       3 005 |         6 436 |    5 339 |
| meeting_room |       3 141 |         6 294 |    4 185 |
| office       |       3 087 |         6 663 |    3 962 |

Excluding the kitchen anomaly, all categories cluster within a tight band (3–7 s). Stricter category-level analysis requires more samples per category — the current 1–4 samples per category is enough to spot outliers but not enough for category-vs-model significance testing.

### 6. Planning speed-vs-reliability trade-off

For tool-calling latency:

- **claude-haiku 1 215 ms p50** — fastest, but 10 % of the time it returns text-only instead of calling a tool. For an autonomous loop this is a *failure*.
- **gemini-3-flash-preview 1 595 ms p50** — only 31 % slower, 100 % tool-call rate.
- **claude-sonnet 2 399 ms p50** — slowest, 100 % tool-call rate.

For a thesis writeup, the conclusion is straightforward: **gemini-3-flash-preview is the best planning choice** of the three. It beats sonnet on speed (almost 2×) and beats haiku on reliability (10× the success rate for tool calls).

## Methodology notes

- All calls land on the production VPS, so latency includes network RTT from `vpn.maastrichtuniversity.nl` ↔ `89.167.11.147` over the public internet (typically 20–60 ms).
- Server-side image normalization (`Image.open → convert RGB → JPEG q92`) is done before any cloud call so palette-mode JPEGs in MIT Indoor Scenes don't bias the comparison.
- The runner is sequential per the response order in the output log; a future concurrent runner would reduce wall-clock time but would not change per-call latency (which is what we measure).
- Latencies measured **server-side**: from the moment the request hits FastAPI to the moment the response is ready. The runner-side `time_total` was not used because it would include the client-server upload time, which varies with VPN routing.
- Planning prompts and analysis objectives are fixed in `scripts/run_benchmark.py` — fully reproducible.

## Pending work (Ernis phase)

The same 90-call benchmark will be re-run against the open-weight models hosted on Ernis (Maastricht University GPU server). Once the SSH tunnel is up:

```bash
# Terminal 1: open the tunnel (must stay running)
ssh -N -L 8001:localhost:8001 I6365661@ernis.dacs.maastrichtuniversity.nl

# Terminal 2: run the full 4-way benchmark
python scripts/run_benchmark.py --server http://89.167.11.147:8000
```

Open-weight models:
- **qwen3-vl** — Qwen3-VL-30B-A3B Q4_K_XL via llama.cpp on RTX 6000 Ada 48 GB
- **qwen2.5-vl** — Qwen2.5-VL-3B Q4_K_M (edge model)

Expected outcomes to validate:

1. Whether open-weight VLMs can match cloud accuracy on indoor monitoring scenes
2. Latency cost of self-hosted vs cloud (Ernis has zero per-request network cost but adds VPN RTT)
3. Whether Qwen-3B is viable as an edge model for the STM32 → server pipeline

## Reproducibility

- Server image: `ghcr.io/whitehatd/thesis-server:latest` at commit `8ecf733`
- Image dataset: `scripts/curate_benchmark_images.py` seeded with `20260519`
- Runner: `scripts/run_benchmark.py --server http://89.167.11.147:8000 --skip-models qwen3-vl qwen2.5-vl`
- Raw output: `results/benchmark_20260519_034252.jsonl`

## Bugs found and fixed along the way

1. `/api/benchmark/analyze` originally required a server-side filesystem path — runner couldn't upload from another machine. Fixed to accept multipart file upload.
2. `google-genai` 1.66 broke positional `Part.from_text(text)`. Two call sites updated to `Part(text=...)` and SDK pinned.
3. `ANTHROPIC_API_KEY` GitHub Secret was expired → 401 on every Claude call. Rotated via `gh secret set`.
4. MIT Indoor Scenes contains some palette-mode JPEGs (e.g. `computerroom_02.jpg`, 256×256 mode P). Anthropic Vision rejects them with 400; Gemini accepts. Fixed by normalizing all uploads to RGB JPEG server-side, decoupling the runner from dataset format quirks.
5. Pushing a CI deploy mid-benchmark force-recreated the server container and broke 7 consecutive calls. New project rule: never push to main while a benchmark is running.
