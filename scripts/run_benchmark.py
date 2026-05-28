#!/usr/bin/env python3
"""
Thesis benchmark runner — analysis + planning across all AI backends.

Calls the thesis FastAPI server's /api/benchmark/* endpoints.
The server must be running before invoking this script.

ANALYSIS: each image is sent to all analysis backends.
PLANNING: each prompt is sent to all planning backends.
Results are saved as JSONL (one row per call) + a summary printed at the end.

Usage:
    # Default: 20 images from scripts/benchmark_images/, all backends
    python scripts/run_benchmark.py

    # Skip qwen3-vl (Ernis not connected yet)
    python scripts/run_benchmark.py --skip-models qwen3-vl

    # Custom image dir or explicit image list
    python scripts/run_benchmark.py --images-dir path/to/frames/
    python scripts/run_benchmark.py --images frame1.jpg frame2.jpg ...

    # Planning only
    python scripts/run_benchmark.py --no-analysis

    # Analysis only
    python scripts/run_benchmark.py --no-planning

    # Against VPS instead of localhost
    python scripts/run_benchmark.py --server https://your-vps-domain

Output:
    results/benchmark_YYYYMMDD_HHMMSS.jsonl
"""

import argparse
import asyncio
import json
import random
import sys
from datetime import datetime
from pathlib import Path

import httpx

# ── Backend lists ─────────────────────────────────────────────────────────────

# model_key values accepted by /api/benchmark/analyze (engine.py routing)
ANALYSIS_MODELS = [
    "claude-haiku",
    "claude-sonnet",
    "claude-sonnet-nothink",   # control arm: Sonnet at T=0.1 without extended thinking
    "gemini-3",
    "qwen3-vl",
    "qwen2.5-vl",
]

# model_key values accepted by /api/benchmark/plan (benchmark_routes.py routing)
PLANNING_MODELS = [
    "claude-haiku",
    "claude-sonnet",
    "claude-sonnet-nothink",   # control arm: Sonnet at T=0.1 without extended thinking
    "gemini-3-flash-preview",
    "qwen3-vl",
    # qwen2.5-vl excluded: empirically verified (2026-05-28) that the 3B model
    # does NOT emit OpenAI tool_calls under llama.cpp --jinja default; it returns
    # conversational text or markdown-fenced Python-style calls. The 30B variant
    # (qwen3-vl) DOES support tool_use cleanly and is included.
]

# ── Planning prompts ─────────────────────────────────────────────────────────
# Realistic natural-language scheduling requests a user would give the IoT system

PLANNING_PROMPTS = [
    # P1-P10: original 10-prompt corpus (kept unchanged for back-compat with old JSONL)
    "Monitor the workspace every 30 minutes for the next 4 hours",
    "Take a photo every hour starting at 9am for 8 hours, stop if nothing changes",
    "Capture images every 15 minutes between 8am and 6pm on weekdays",
    "Check the plant every 2 hours during daylight hours",
    "Monitor the entrance every 20 minutes and send an alert if someone arrives",
    "Take 3 photos per hour for the next 2 hours to track lighting changes",
    "Capture the workspace at 9am, 12pm, 3pm, and 6pm every day",
    "Monitor continuously every 10 minutes for 1 hour to detect any activity",
    "Schedule daily monitoring at 8am and 5pm for the next week",
    "Take a photo immediately and then every 45 minutes for 3 hours",
    # P11-P13: additional t=0-class probes added 2026-05-28 to strengthen the
    # constraint-preservation finding from N=1 prompt to N=4. All four phrasings
    # combine an immediate capture with a recurring schedule; the question is
    # whether each backend preserves the t=0 component when forwarding to the
    # planning engine. If only P10 fails the t=0 axis, the failure is anecdotal;
    # if P10-P13 all fail, the failure is a robust LLM behavior pattern.
    "Take a photo right now and then capture every 30 minutes for 2 hours",
    "Start with a baseline capture, then take one every 1 hour for 4 hours",
    "Take an immediate snapshot followed by hourly checks for the next 3 hours",
]


# ── Helpers ───────────────────────────────────────────────────────────────────

def select_images(images_dir: Path, n: int) -> list[Path]:
    """Select up to n images from images_dir, sorted for reproducibility."""
    all_images = sorted(images_dir.glob("*.jpg")) + sorted(images_dir.glob("*.jpeg"))
    if not all_images:
        print(f"ERROR: no .jpg/.jpeg files in {images_dir}", file=sys.stderr)
        sys.exit(1)
    if len(all_images) <= n:
        return all_images
    # Sample evenly — reproducible without randomness
    step = len(all_images) / n
    return [all_images[int(i * step)] for i in range(n)]


async def call_analyze(
    client: httpx.AsyncClient,
    server: str,
    image_path: Path,
    model_key: str,
    objective: str,
    rep_index: int = 0,
) -> dict:
    """Send one analyze request to the benchmark server.

    rep_index is appended to the objective as a hidden marker (e.g.,
    "[rep 3] Monitor the workspace"). This defeats Anthropic/Gemini prompt
    caching that would otherwise make reps 1-N return artificially low
    latencies from a warm cache. The marker is text-only and does not affect
    the visual analysis. Image bytes are also untouched, so vision is
    deterministic on the input — only the text portion of the prompt differs
    between reps.
    """
    try:
        image_bytes = image_path.read_bytes()
        # Per-rep + per-model nonce as a prefix to objective (token-cheap, cache-defeating).
        # The model_key MUST be in the nonce: claude-sonnet and claude-sonnet-nothink both
        # hit the same underlying model (claude-sonnet-4-6), so without arm-distinct nonces
        # Anthropic's prompt cache serves the first arm's (thinking, T=1.0) response to the
        # second arm (no-thinking, T=0.1), silently invalidating the control comparison.
        # Tagging by model_key makes every arm cache-distinct AND gives uniform treatment.
        objective_with_nonce = f"[rep {rep_index}|{model_key}] {objective}"
        r = await client.post(
            f"{server}/api/benchmark/analyze",
            files={"file": (image_path.name, image_bytes, "image/jpeg")},
            data={"model_key": model_key, "objective": objective_with_nonce},
            timeout=120.0,
        )
        r.raise_for_status()
        return r.json()
    except Exception as e:
        return {
            "model_key": model_key,
            "image_path": str(image_path),
            "success": False,
            "error": str(e),
            "latency_ms": 0,
            "result": None,
        }


async def call_plan(
    client: httpx.AsyncClient,
    server: str,
    prompt: str,
    model_key: str,
    rep_index: int = 0,
) -> dict:
    # Per-rep nonce defeats prompt caching (Anthropic + Gemini). The nonce is
    # prepended as a hidden marker so each rep sends a textually-different
    # prompt; the actual scheduling intent is identical. Without this, reps
    # 1..N would hit cache and report artificially low latencies.
    try:
        # Per-rep + per-model nonce (see call_analyze for the cache-contamination
        # rationale — claude-sonnet vs claude-sonnet-nothink share model claude-sonnet-4-6).
        prompt_with_nonce = f"[rep {rep_index}|{model_key}] {prompt}"
        r = await client.post(
            f"{server}/api/benchmark/plan",
            json={"prompt": prompt_with_nonce, "model_key": model_key},
            timeout=60.0,
        )
        r.raise_for_status()
        return r.json()
    except Exception as e:
        return {
            "model_key": model_key,
            "prompt": prompt,
            "success": False,
            "error": str(e),
            "latency_ms": 0,
        }


def print_summary(results: list[dict]) -> None:
    analysis = [r for r in results if r.get("_type") == "analysis"]
    planning = [r for r in results if r.get("_type") == "planning"]

    if analysis:
        print("\n=== Analysis — latency (ms) ===")
        models = sorted({r["model_key"] for r in analysis})
        print(f"  {'model':<28}  {'n':>4}  {'ok':>4}  {'p50':>7}  {'p95':>7}  {'mean':>7}")
        print(f"  {'-'*28}  {'-'*4}  {'-'*4}  {'-'*7}  {'-'*7}  {'-'*7}")
        for m in models:
            rows = [r for r in analysis if r["model_key"] == m]
            ok = [r for r in rows if r.get("success")]
            if ok:
                lats = sorted(r["latency_ms"] for r in ok)
                p50 = lats[len(lats) // 2]
                p95 = lats[max(0, int(len(lats) * 0.95) - 1)]
                mean = sum(lats) / len(lats)
                print(f"  {m:<28}  {len(rows):>4}  {len(ok):>4}  {p50:>7.0f}  {p95:>7.0f}  {mean:>7.0f}")
            else:
                print(f"  {m:<28}  {len(rows):>4}  {0:>4}  {'—':>7}  {'—':>7}  {'—':>7}")

    if planning:
        print("\n=== Planning — success rate ===")
        models = sorted({r["model_key"] for r in planning})
        print(f"  {'model':<28}  {'n':>4}  {'ok':>4}  {'p50 ms':>8}")
        for m in models:
            rows = [r for r in planning if r["model_key"] == m]
            ok = [r for r in rows if r.get("success")]
            lats = sorted(r["latency_ms"] for r in ok) if ok else []
            p50 = f"{lats[len(lats)//2]:.0f}" if lats else "—"
            print(f"  {m:<28}  {len(rows):>4}  {len(ok):>4}  {p50:>8}")


# ── Main ─────────────────────────────────────────────────────────────────────

async def main() -> None:
    repo_root = Path(__file__).parent.parent

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", default="http://localhost:8000", help="Base URL of the running thesis server")
    parser.add_argument(
        "--images-dir",
        type=Path,
        default=repo_root / "scripts" / "benchmark_images",
        help="Directory containing benchmark JPEG frames (default: scripts/benchmark_images/)",
    )
    parser.add_argument("--images", nargs="+", type=Path, help="Explicit list of image files (overrides --images-dir)")
    parser.add_argument("--n", type=int, default=20, help="Max images to use from --images-dir (default: 20)")
    parser.add_argument("--skip-models", nargs="*", default=[], metavar="MODEL", help="Model keys to skip (e.g. qwen3-vl)")
    parser.add_argument("--objective", default="Monitor the workspace for activity and safety", help="Monitoring objective passed to each analysis call")
    parser.add_argument("--no-analysis", action="store_true", help="Skip analysis benchmark")
    parser.add_argument("--no-planning", action="store_true", help="Skip planning benchmark")
    parser.add_argument("--out-dir", type=Path, default=repo_root / "results", help="Output directory for JSONL results")
    parser.add_argument("--seed", type=int, default=42, help="Random seed for reproducible image sampling")
    parser.add_argument(
        "--reps",
        type=int,
        default=1,
        help="Repetitions per (image,model) and (prompt,model) pair. Default 1. "
             "Each row gets _rep_index field 0..reps-1. Use --reps 10 for proper "
             "statistical variance estimation.",
    )
    args = parser.parse_args()

    random.seed(args.seed)
    skip = set(args.skip_models)
    analysis_models = [m for m in ANALYSIS_MODELS if m not in skip]
    planning_models = [m for m in PLANNING_MODELS if m not in skip]

    # Resolve image list
    if args.images:
        images = [Path(p).resolve() for p in args.images]
        missing = [p for p in images if not p.exists()]
        if missing:
            print(f"ERROR: image(s) not found: {missing}", file=sys.stderr)
            sys.exit(1)
    else:
        if not args.images_dir.exists():
            print(f"ERROR: --images-dir not found: {args.images_dir}", file=sys.stderr)
            print("Run extract_frames.py first to populate it.", file=sys.stderr)
            sys.exit(1)
        images = select_images(args.images_dir, args.n)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_file = args.out_dir / f"benchmark_{ts}.jsonl"

    reps = max(1, args.reps)
    total_analysis = len(images) * len(analysis_models) * reps if not args.no_analysis else 0
    total_planning = len(PLANNING_PROMPTS) * len(planning_models) * reps if not args.no_planning else 0
    total = total_analysis + total_planning

    print(f"Thesis benchmark — {datetime.now().strftime('%Y-%m-%d %H:%M')}")
    print(f"  Images   : {len(images)} ({args.images_dir if not args.images else 'explicit list'})")
    print(f"  Reps     : {reps} per (image,model) and (prompt,model) pair")
    print(f"  Analysis : {total_analysis} calls ({len(analysis_models)} models × {len(images)} images × {reps} reps)")
    print(f"  Planning : {total_planning} calls ({len(planning_models)} models × {len(PLANNING_PROMPTS)} prompts × {reps} reps)")
    print(f"  Server   : {args.server}")
    print(f"  Output   : {out_file}")
    print()

    async with httpx.AsyncClient() as client:
        # Health check
        try:
            r = await client.get(f"{args.server}/health", timeout=5.0)
            r.raise_for_status()
            print(f"Server OK ({args.server})\n")
        except Exception as e:
            print(f"ERROR: server not reachable at {args.server}: {e}", file=sys.stderr)
            print("Start the server: cd server && uvicorn app.main:app --reload", file=sys.stderr)
            sys.exit(1)

        all_results: list[dict] = []
        done = 0

        with open(out_file, "w", encoding="utf-8") as f:

            # ── Analysis benchmark ──
            if not args.no_analysis:
                print(f"[analysis] {len(images)} images × {analysis_models} × {reps} reps")
                for rep_idx in range(reps):
                    for img in images:
                        for model_key in analysis_models:
                            result = await call_analyze(client, args.server, img, model_key, args.objective, rep_index=rep_idx)
                            done += 1
                            row = {
                                "_type": "analysis",
                                "_timestamp": datetime.now().isoformat(),
                                "_image": img.name,
                                "_rep_index": rep_idx,
                                **result,
                            }
                            f.write(json.dumps(row, ensure_ascii=False) + "\n")
                            f.flush()
                            all_results.append(row)

                            ok = result.get("success", False)
                            lat = result.get("latency_ms", 0)
                            status = f"{lat:.0f}ms" if ok else f"FAIL: {str(result.get('error', ''))[:60]}"
                            print(f"  [{done:4}/{total}] r{rep_idx} {model_key:<22} {img.name:<20} {status}")

            # ── Planning benchmark ──
            if not args.no_planning:
                print(f"\n[planning] {len(PLANNING_PROMPTS)} prompts × {planning_models} × {reps} reps")
                for rep_idx in range(reps):
                    for i, prompt in enumerate(PLANNING_PROMPTS):
                        for model_key in planning_models:
                            result = await call_plan(client, args.server, prompt, model_key, rep_index=rep_idx)
                            done += 1
                            row = {
                                "_type": "planning",
                                "_timestamp": datetime.now().isoformat(),
                                "_prompt_idx": i,
                                "_rep_index": rep_idx,
                                "prompt": prompt,
                                **result,
                            }
                            f.write(json.dumps(row, ensure_ascii=False) + "\n")
                            f.flush()
                            all_results.append(row)

                            ok = result.get("success", False)
                            lat = result.get("latency_ms", 0)
                            tool = result.get("tool_name") or "—"
                            status = f"{lat:.0f}ms -> {tool}" if ok else f"FAIL: {str(result.get('error', ''))[:50]}"
                            print(f"  [{done:4}/{total}] r{rep_idx} plan/{model_key:<22} prompt[{i}] {status}")

    print(f"\nDone. {done}/{total} calls completed -> {out_file}")
    print_summary(all_results)


if __name__ == "__main__":
    asyncio.run(main())
