#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inference burst driver for the LLM-vs-renderer GPU contention measurement (#782).

Drives a local OpenAI-compatible endpoint on a **time-phased schedule** — idle baseline, then
alternating inference bursts and idle gaps, then an idle tail — and records exactly when each phase
ran, in wall-clock epoch milliseconds.

That timestamp is the whole point. The game, running concurrently, writes per-frame samples stamped
on the same clock (`--frame-stats-json`, engine/perf/FrameStatsRecorder.h); analyze.py then joins
the two and answers the question #782 actually asks: what did the frame time do *while the model was
running*, versus while it was idle. Neither file answers that alone.

Why a separate script rather than a mode of tools/ai_eval/ai_eval.py: that harness measures per-case
ACCURACY, strictly sequentially, one request in flight. This is a load generator with no scoring and
optional concurrency. The one thing they genuinely share — the HTTP edge — is imported from it rather
than copied, so there is one place where a request is formed.

This never runs in CI and CI never requires a model (docs/developer/ai-architecture.md §7). The pure scheduling
logic is unit-tested in tests/test_gpu_contention.py.

    python3 tools/gpu_contention/driver.py --model qwen2.5-coder:14b \
        --base-url http://localhost:11434 --bursts 5 --burst-seconds 20

Stdlib only. The API key is read from the environment, never a flag.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
AI_EVAL = REPO_ROOT / "tools" / "ai_eval" / "ai_eval.py"
SUITES = REPO_ROOT / "tools" / "ai_eval" / "suites"

SCHEMA_VERSION = 1


def _load_ai_eval():
    """Import tools/ai_eval/ai_eval.py by path (it is a script, not an installed module).

    Through the shared loader (#1265), which also fixes what this copy was missing: it never
    registered the module in sys.modules, so a dataclass in the script under load could not resolve
    its annotations and a cross-import would have loaded a second copy.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "common"))
    from pyload import load_script_module  # noqa: E402

    return load_script_module("ai_eval", AI_EVAL)


# ---- schedule ----------------------------------------------------------------------------------


def build_schedule(
    start_ms,
    idle_seconds,
    bursts,
    burst_seconds,
    gap_seconds,
    tail_seconds,
    sustained_seconds=0,
):
    """The planned phase windows, in epoch ms. Pure — no clock, no I/O.

    Layout: idle baseline, then either one sustained burst or `bursts` x (burst, gap), then an idle
    tail. The trailing gap after the last burst is dropped in favour of the tail (they would be the
    same thing measured twice).

    The idle phases are not padding. The comparison "frame time during inference vs frame time
    without it" needs a same-session, same-scene baseline: comparing against a separate run would
    fold in every difference between two launches.
    """
    phases = []
    t = float(start_ms)

    def add(kind, index, seconds):
        nonlocal t
        if seconds <= 0:
            return
        phases.append({"phase": kind, "index": index, "start_ms": t, "end_ms": t + seconds * 1000.0})
        t += seconds * 1000.0

    add("idle", 0, idle_seconds)
    if sustained_seconds > 0:
        add("burst", 0, sustained_seconds)
    else:
        for i in range(max(0, bursts)):
            add("burst", i, burst_seconds)
            if i < bursts - 1:
                add("gap", i, gap_seconds)
    add("tail", 0, tail_seconds)
    return phases


def schedule_duration_s(phases):
    """Total planned wall-clock seconds for a schedule (0 for an empty one)."""
    if not phases:
        return 0.0
    return (phases[-1]["end_ms"] - phases[0]["start_ms"]) / 1000.0


# ---- workload prompts --------------------------------------------------------------------------


def load_workload(name):
    """(system_prompt, [user_prompt, ...]) for a workload, read from the ai_eval suites.

    Using the real suites keeps the load honest: `intent` is prompt-eval dominated (the wingman
    grammar is ingested every call and the answer is ~12 tokens — see #769), while `mission` is
    generation dominated. Those two profiles stress a GPU very differently, and a synthetic
    "write me a poem" prompt would represent neither.
    """
    path = SUITES / f"{name}.json"
    suite = json.loads(path.read_text(encoding="utf-8"))
    system = suite["system_prompt"]
    if name == "intent":
        prompts = [c["utterance"] for c in suite["cases"]]
    elif name == "mission":
        prompts = [c["brief"] for c in suite["cases"]]
    else:
        prompts = [json.dumps(c.get("snapshot", {})) for c in suite["cases"]]
    if not prompts:  # pragma: no cover - suite would be malformed
        raise RuntimeError(f"suite {name} has no cases")
    return system, prompts


# ---- endpoint-side memory probe ----------------------------------------------------------------


def probe_loaded_models(base_url, timeout=5.0):
    """Ollama's /api/ps — the loaded models and their VRAM footprint, or None.

    Best-effort and Ollama-specific: llama-server and LM Studio expose nothing equivalent, so a None
    here means "ask the OS", which is what the per-OS runner scripts do. The model's own footprint
    lives in the inference server's process and is invisible to the game's VK_EXT_memory_budget
    view, so without this the VRAM story is only half told.
    """
    url = base_url.rstrip("/") + "/api/ps"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except Exception:
        return None
    models = []
    for m in body.get("models", []):
        models.append(
            {
                "name": m.get("name") or m.get("model"),
                "size_bytes": m.get("size"),
                "size_vram_bytes": m.get("size_vram"),
            }
        )
    return models


# ---- burst execution ---------------------------------------------------------------------------


class _BurstResult:
    def __init__(self):
        self.latencies = []
        self.errors = []
        self.last_completion_ms = 0.0
        self.lock = threading.Lock()


def _burst_worker(cfg, ai_eval, prompts, offset, deadline_mono, result):
    """Loop requests until the burst deadline. Requests in flight at the deadline finish."""
    i = offset
    while time.monotonic() < deadline_mono:
        prompt = prompts[i % len(prompts)]
        i += 1
        try:
            _text, elapsed = ai_eval.chat_completion(
                cfg["base_url"],
                cfg["api_key"],
                cfg["model"],
                cfg["system"],
                prompt,
                cfg["timeout"],
                cfg["json_mode"],
                cfg["merge_system"],
            )
            done = time.time() * 1000.0
            with result.lock:
                result.latencies.append(elapsed)
                result.last_completion_ms = max(result.last_completion_ms, done)
        except RuntimeError as e:
            with result.lock:
                result.errors.append(str(e)[:200])


def run_burst(cfg, ai_eval, prompts, seconds, concurrency):
    """Drive `concurrency` request loops for `seconds`. Returns the observed window + stats."""
    start_ms = time.time() * 1000.0
    deadline = time.monotonic() + seconds
    result = _BurstResult()
    threads = [
        threading.Thread(target=_burst_worker, args=(cfg, ai_eval, prompts, k, deadline, result), daemon=True)
        for k in range(max(1, concurrency))
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    # The window ends at the last COMPLETION, not at the deadline: a request still decoding is still
    # loading the GPU, and closing the window early would attribute its frames to idle.
    end_ms = max(result.last_completion_ms, time.time() * 1000.0)
    return {
        "start_ms": start_ms,
        "end_ms": end_ms,
        "requests": len(result.latencies),
        "errors": len(result.errors),
        "error_samples": result.errors[:3],
        "latency_s": ai_eval.summarize_latencies(result.latencies),
    }


def sleep_phase(seconds):
    """Idle window, recorded on the same clock as the bursts."""
    start_ms = time.time() * 1000.0
    time.sleep(max(0.0, seconds))
    return {"start_ms": start_ms, "end_ms": time.time() * 1000.0, "requests": 0, "errors": 0}


# ---- main --------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description="LLM inference burst driver for GPU-contention measurement (#782)")
    ap.add_argument("--base-url", default=os.environ.get("FL_AI_BASE_URL", "http://localhost:11434"))
    ap.add_argument("--model", required=True, help="model id as the endpoint names it")
    ap.add_argument("--api-key-env", default="FL_AI_API_KEY", help="env var holding the API key (never a flag)")
    ap.add_argument("--workload", choices=["intent", "mission", "ops"], default="intent")
    ap.add_argument("--concurrency", type=int, default=1, help="parallel request loops during a burst")
    ap.add_argument("--idle-seconds", type=float, default=60.0, help="baseline idle window before the first burst")
    ap.add_argument("--bursts", type=int, default=5)
    ap.add_argument("--burst-seconds", type=float, default=20.0)
    ap.add_argument("--gap-seconds", type=float, default=20.0)
    ap.add_argument("--tail-seconds", type=float, default=30.0, help="idle window after the last burst")
    ap.add_argument("--sustained-seconds", type=float, default=0.0, help="one long burst instead of a burst train")
    ap.add_argument("--merge-system", action="store_true", help="fold the system prompt into the user turn")
    ap.add_argument("--timeout", type=float, default=180.0, help="per-request timeout, seconds")
    ap.add_argument("--json-mode", action="store_true", help="request response_format=json_object")
    ap.add_argument("--out", default="", help="output JSON path (default: results/driver_<utc-ts>.json)")
    ap.add_argument("--print-schedule", action="store_true", help="print the planned duration in seconds and exit")
    args = ap.parse_args(argv)

    planned = build_schedule(
        0.0,
        args.idle_seconds,
        args.bursts,
        args.burst_seconds,
        args.gap_seconds,
        args.tail_seconds,
        args.sustained_seconds,
    )
    total_s = schedule_duration_s(planned)
    if args.print_schedule:
        # Lets the per-OS runner size the game's --run-seconds off the same source of truth, rather
        # than re-deriving the arithmetic in bash and again in PowerShell.
        print(f"{total_s:.1f}")
        return 0

    ai_eval = _load_ai_eval()
    system, prompts = load_workload(args.workload)
    cfg = {
        "base_url": args.base_url,
        "api_key": os.environ.get(args.api_key_env, ""),
        "model": args.model,
        "system": system,
        "timeout": args.timeout,
        "json_mode": args.json_mode,
        "merge_system": args.merge_system,
    }

    print(f"[gpu-contention] endpoint {args.base_url} model {args.model} workload {args.workload}", flush=True)
    print(f"[gpu-contention] planned duration {total_s:.0f} s ({len(planned)} phases)", flush=True)

    # Warm-up probe. A cold 14B costs ~55 s to load on the reference instance (#769) and Ollama
    # evicts an idle model after 5 minutes by default — an unpinned model would load INSIDE the
    # first burst and be measured as contention rather than as what it is. Pin the model
    # (OLLAMA_KEEP_ALIVE) and pay the load here, where it is recorded separately.
    ps_before = probe_loaded_models(args.base_url)
    probe_start = time.monotonic()
    try:
        ai_eval.chat_completion(
            cfg["base_url"],
            cfg["api_key"],
            cfg["model"],
            cfg["system"],
            prompts[0],
            cfg["timeout"],
            cfg["json_mode"],
            cfg["merge_system"],
        )
        probe_s = time.monotonic() - probe_start
        probe_error = ""
    except RuntimeError as e:
        probe_s = time.monotonic() - probe_start
        probe_error = str(e)[:300]
        print(f"[gpu-contention] warm-up FAILED after {probe_s:.1f} s: {probe_error}", file=sys.stderr)
        return 2
    print(f"[gpu-contention] warm-up ok in {probe_s:.1f} s — starting schedule", flush=True)

    phases = []
    for p in planned:
        kind, index = p["phase"], p["index"]
        seconds = (p["end_ms"] - p["start_ms"]) / 1000.0
        print(f"[gpu-contention] {kind}[{index}] {seconds:.0f} s", flush=True)
        if kind == "burst":
            rec = run_burst(cfg, ai_eval, prompts, seconds, args.concurrency)
        else:
            rec = sleep_phase(seconds)
        rec["phase"] = kind
        rec["index"] = index
        phases.append(rec)

    ps_after = probe_loaded_models(args.base_url)
    report = {
        "schema_version": SCHEMA_VERSION,
        "endpoint": args.base_url,
        "model": args.model,
        "workload": args.workload,
        "concurrency": args.concurrency,
        "json_mode": args.json_mode,
        "merge_system": args.merge_system,
        "model_load_probe_s": round(probe_s, 3),
        "model_load_probe_error": probe_error,
        "loaded_models_before": ps_before,
        "loaded_models_after": ps_after,
        "started_epoch_ms": phases[0]["start_ms"] if phases else time.time() * 1000.0,
        "duration_s": round(schedule_duration_s(phases), 3),
        "total_requests": sum(p["requests"] for p in phases),
        "total_errors": sum(p["errors"] for p in phases),
        "phases": phases,
    }

    out = (
        Path(args.out)
        if args.out
        else Path(__file__).resolve().parent
        / "results"
        / ("driver_" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + ".json")
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"[gpu-contention] {report['total_requests']} requests, {report['total_errors']} errors", flush=True)
    print(f"[gpu-contention] wrote {out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
