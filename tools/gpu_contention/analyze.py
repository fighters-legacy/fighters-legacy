#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Join a frame-stats report against an inference-burst report and report the contention (#782).

Inputs:
    --frame-stats  the game's --frame-stats-json document (engine/perf/FrameStatsRecorder.h)
    --driver       driver.py's phase report

Both stamp wall-clock epoch milliseconds, so every frame can be attributed to the phase that was
running when it rendered. The output is the comparison #782 asks for: frame time and VRAM while
the model was inferring, against the same session's idle baseline.

Everything here is pure — file I/O happens only in main() — so the analysis is unit-tested in
tests/test_gpu_contention.py without a GPU, a model, or a network.

    python3 tools/gpu_contention/analyze.py --frame-stats frames.json --driver driver.json
"""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import sys
import time
from pathlib import Path

# A hitch is measured RELATIVE to the run's own idle baseline: a frame taking more than this
# multiple of the idle median frame interval. The threshold used to be a fixed 33.3 ms, which is
# degenerate on any client whose ordinary cadence already sits at or past it — on a 60 Hz panel
# vsync-locked to 30 fps (a 33.3 ms idle cadence) it scored 7757 of 14200 *idle* frames as hitches,
# in phases with no inference running at all (#1022). Relative to the baseline, "hitch" means the
# same thing on a 240 Hz client and a 30 fps one: a frame that took markedly longer than this
# machine's normal one.
#
# The stall threshold stays ABSOLUTE. It is the band a player reads as a stutter rather than as
# general slowness, and that is a property of human perception, not of the client's frame rate.
HITCH_IDLE_MULTIPLE = 2.0
STALL_MS = 50.0

# Frames within this many seconds of a phase start are discarded. A phase boundary is not
# instantaneous for the renderer: the first requests of a burst are still being scheduled, and the
# GPU is still draining the previous phase's work. Attributing those frames to either side smears
# the very difference the run is measuring.
SETTLE_S = 5.0

# A frame shorter than this fraction of the run's idle cadence is a CATCH-UP frame: on a vsync-
# limited client with images queued, a frame that waited an extra refresh interval is followed by
# one that was already finished and returns immediately. The share of these is how you tell a
# renderer that WAITED from a renderer whose work got SLOWER (#1025) — slower work cannot produce
# short frames, and it lowers throughput, which a wait followed by a drain does not.
SHORT_FRAME_FRACTION = 0.5


# ---- pure analysis -----------------------------------------------------------------------------


def summarize_ms(values):
    """min/mean/p50/p95/p99/max over a list. Empty -> zeros. Nearest-rank, like fl::computeStats."""
    if not values:
        return {"min": 0.0, "mean": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0, "n": 0}
    s = sorted(values)
    n = len(s)

    def rank(q):
        return s[min(n - 1, max(0, int(q * (n - 1))))]

    return {
        "min": round(s[0], 3),
        "mean": round(statistics.fmean(s), 3),
        "p50": round(statistics.median(s), 3),
        "p95": round(rank(0.95), 3),
        "p99": round(rank(0.99), 3),
        "max": round(s[-1], 3),
        "n": n,
    }


def worst_percent_mean(values, fraction=0.01):
    """Mean of the slowest `fraction` of frames — what the run felt like at its worst.

    A p99 hides how bad the tail is; two runs with the same p99 can have very different worst-1%
    means, and the difference is exactly "an occasional blip" versus "it locks up".
    """
    if not values:
        return 0.0
    s = sorted(values, reverse=True)
    k = max(1, int(len(s) * fraction))
    return round(statistics.fmean(s[:k]), 3)


def classify_samples(samples, phases, settle_s=SETTLE_S):
    """Bucket frame samples by the driver phase running when they rendered.

    `samples` are [t_ms, frame_ms, gpu_ms, gpu_mem_used_mb] rows; `phases` are driver phase
    records with start_ms/end_ms. Returns (buckets, outside), where buckets is
    {phase_kind: {"frame_ms": [...], "gpu_ms": [...], "mem_mb": [...], "windows": {index: count}}}
    and `outside` counts frames belonging to no window.

    A large `outside` count is NORMAL and not an error: the runners deliberately start the game
    before the schedule and let it outlive the schedule, so that neither end of the measurement is
    truncated. What matters is per-window COVERAGE, which the "windows" counts carry.

    Idle and gap are kept separate rather than merged into one "not bursting" bucket: a gap follows
    a burst, so it can still carry recovery effects (thermal throttling, a driver settling down).
    If they disagree, that is a finding, not noise to average away.
    """
    buckets = {}
    outside = 0
    for row in samples:
        if len(row) < 4:
            continue
        t, frame_ms, gpu_ms, mem_mb = row[0], row[1], row[2], row[3]
        hit = None
        for p in phases:
            if p["start_ms"] + settle_s * 1000.0 <= t < p["end_ms"]:
                hit = p
                break
        if hit is None:
            outside += 1
            continue
        b = buckets.setdefault(hit["phase"], {"frame_ms": [], "gpu_ms": [], "mem_mb": [], "windows": {}})
        b["frame_ms"].append(frame_ms)
        b["gpu_ms"].append(gpu_ms)
        b["mem_mb"].append(mem_mb)
        idx = hit.get("index", 0)
        b["windows"][idx] = b["windows"].get(idx, 0) + 1
    return buckets, outside


def residual_ms(frame_ms_values, gpu_ms_values):
    """Per-frame `frame_ms - gpu_ms`: the part of the frame the renderer was NOT executing on the GPU.

    `gpu_ms` is the renderer's own timestamp-query span, so the remainder is CPU frame work, waiting
    to be scheduled, and present. Splitting the burst-vs-idle delta across these two is what
    separates "the renderer's work got slower" from "the renderer waited" (#1025) — two mechanisms
    that look identical in frame time and call for completely different fixes.

    Computed PER FRAME, then summarised, so it is a real distribution rather than a subtraction of
    two summaries. Note the split is exact only at the mean, where the two components are additive;
    at p95/p99 each component's percentile is taken independently, so the rows localise the tail
    rather than forming an identity.

    Pairs positionally: both lists come from the same bucket and are appended together in
    classify_samples, so index i is one frame. Extra entries on either side are ignored.
    """
    return [f - g for f, g in zip(frame_ms_values, gpu_ms_values)]


def short_frame_pct(frame_ms_values, idle_median_ms, fraction=SHORT_FRAME_FRACTION):
    """Share of frames shorter than `fraction` x the idle cadence — the catch-up drain (#1025).

    None when there is no idle baseline to calibrate against, on the same principle as the hitch
    threshold: "could not measure" is not "measured zero".
    """
    if not frame_ms_values or not idle_median_ms or idle_median_ms <= 0:
        return None
    cutoff = idle_median_ms * fraction
    return round(100.0 * sum(1 for v in frame_ms_values if v < cutoff) / len(frame_ms_values), 2)


def drain_after_hitch_pct(frame_ms_values, hitch_ms, idle_median_ms, fraction=SHORT_FRAME_FRACTION):
    """Of the frames over the hitch threshold, the share whose successor was a catch-up frame.

    The unconditional short-frame share says short frames and long frames both became common; this
    says they are the SAME event — a frame that waited, followed by the queued image that was
    already finished. That is what a slowdown cannot produce, and it is why the pair is reported
    rather than either alone (#1025).

    Reuses the run's calibrated hitch threshold rather than inventing a second "long" cutoff, so
    "hitch" means one thing in the report. None when either calibration is unavailable, or when the
    phase contains no hitch to condition on — an empty conditional is not zero.
    """
    if not frame_ms_values or not hitch_ms or not idle_median_ms or idle_median_ms <= 0:
        return None
    cutoff = idle_median_ms * fraction
    pairs = [(a, b) for a, b in zip(frame_ms_values, frame_ms_values[1:]) if a > hitch_ms]
    if not pairs:
        return None
    return round(100.0 * sum(1 for _a, b in pairs if b < cutoff) / len(pairs), 2)


def frames_per_second(frame_ms_values):
    """Throughput over the frames in a bucket. A genuine slowdown lowers it; a wait+drain does not."""
    if not frame_ms_values:
        return 0.0
    total_s = sum(frame_ms_values) / 1000.0
    if total_s <= 0:
        return 0.0
    return round(len(frame_ms_values) / total_s, 2)


def hitch_rate_per_min(frame_ms_values, threshold_ms):
    """Frames over `threshold_ms` per minute of the time those frames actually span.

    Rate rather than count: the baseline and burst buckets hold different numbers of frames by
    construction (a burst window is shorter, and slower frames mean fewer of them), so raw counts
    are not comparable between them. The report prints both.
    """
    if not frame_ms_values:
        return 0.0
    total_s = sum(frame_ms_values) / 1000.0
    if total_s <= 0:
        return 0.0
    over = sum(1 for v in frame_ms_values if v > threshold_ms)
    return round(over / (total_s / 60.0), 2)


def median_frame_ms(frame_ms_values):
    """The run's typical frame interval. None when there are no frames to characterise."""
    if not frame_ms_values:
        return None
    return round(statistics.median(frame_ms_values), 3)


def hitch_threshold_ms(idle_median_ms, multiple=HITCH_IDLE_MULTIPLE):
    """The hitch threshold for a run: `multiple` × the idle baseline's median frame interval.

    Returns None when there is no idle baseline to calibrate against. The caller then reports the
    hitch metrics as unavailable rather than substituting a fixed number, because "we could not
    measure it" and "it measured zero" are different findings — and a fixed number is precisely
    what #1022 removed.
    """
    if not idle_median_ms or idle_median_ms <= 0:
        return None
    return round(idle_median_ms * multiple, 3)


def model_vram_mb(driver):
    """The model's own VRAM footprint in MB from the endpoint probe, or None if unavailable."""
    for key in ("loaded_models_after", "loaded_models_before"):
        models = driver.get(key)
        if not models:
            continue
        want = driver.get("model")
        for m in models:
            if m.get("size_vram_bytes") and (want is None or m.get("name") == want):
                return round(m["size_vram_bytes"] / (1024.0 * 1024.0), 1)
        for m in models:  # any loaded model, if the name did not match exactly
            if m.get("size_vram_bytes"):
                return round(m["size_vram_bytes"] / (1024.0 * 1024.0), 1)
    return None


def sanity_warnings(frame_report, driver, buckets, samples, phases, settle_s=SETTLE_S):
    """Conditions that invalidate the numbers rather than merely qualify them.

    Deliberately NOT a warning: frames falling outside every phase window. The runners bracket the
    schedule with margin on both ends, so most frames in a short schedule are legitimately outside
    it. Warning on that would fire on every correct run, and a check that cries wolf on success is
    worse than no check — it teaches the reader to ignore the warnings block.

    What is checked instead is COVERAGE: did the game actually render throughout every burst, and
    did the recording survive into the final phase.
    """
    warns = []
    burst = buckets.get("burst", {})
    if not burst.get("frame_ms"):
        warns.append(
            "No frames attributed to any burst window. The game was not rendering during "
            "inference — check that it launched, reached Flight, and outlived the driver."
        )
    else:
        # Every burst window must have frames in it. One empty window means the client died or
        # stalled part-way through, and the surviving windows would silently stand in for the run.
        expected = {
            p.get("index", 0)
            for p in phases
            if p["phase"] == "burst" and p["end_ms"] - p["start_ms"] > settle_s * 1000.0
        }
        missing = sorted(expected - set(burst.get("windows", {})))
        if missing:
            warns.append(
                f"No frames recorded during burst window(s) {missing} — the client stopped rendering "
                "part-way through the schedule."
            )
    if not buckets.get("idle", {}).get("frame_ms"):
        warns.append("No frames attributed to the idle baseline; there is nothing to compare against.")
    if samples and phases:
        # Did the recording survive into the final phase? Tested against the final phase's USABLE
        # window (its start plus the settle period) rather than against the schedule's last
        # millisecond: frames arrive at ~16 ms intervals and the settle period is discarded anyway,
        # so an exact-endpoint test would flag correct runs over sampling granularity.
        last_t = max(row[0] for row in samples if len(row) >= 4)
        final_usable = phases[-1]["start_ms"] + settle_s * 1000.0
        if last_t < final_usable:
            warns.append(
                f"Frame recording stopped {(final_usable - last_t) / 1000.0:.0f} s before the final "
                f"'{phases[-1]['phase']}' phase — the client died early or --run-seconds was too short."
            )
    if driver.get("total_errors"):
        warns.append(f"The driver logged {driver['total_errors']} request errors; the burst load was not clean.")
    if frame_report.get("frames", 0) and frame_report.get("gpu_ms", {}).get("max", 0) == 0:
        warns.append(
            "GPU timestamps read 0 — this device/driver does not support timestamp queries, so the "
            "gpu_ms columns are empty (frame_ms is still valid)."
        )
    return warns


def contention_metrics(frame_report, driver, settle_s=SETTLE_S, scene="", label=""):
    """The full report: per-phase distributions, baseline-vs-burst deltas, VRAM headroom."""
    samples = frame_report.get("samples", [])
    phases = driver.get("phases", [])
    buckets, outside = classify_samples(samples, phases, settle_s)

    # The hitch threshold is calibrated once, off the idle baseline, and then applied to EVERY phase
    # — including idle itself. Calibrating per phase would define the burst's hitches in terms of the
    # burst's own slowness and could not show contention at all.
    idle_median_ms = median_frame_ms(buckets.get("idle", {}).get("frame_ms", []))
    hitch_ms = hitch_threshold_ms(idle_median_ms)

    per_phase = {}
    for kind, b in buckets.items():
        per_phase[kind] = {
            "frame_ms": summarize_ms(b["frame_ms"]),
            "gpu_ms": summarize_ms(b["gpu_ms"]),
            # frame_ms minus the renderer's own GPU execution span, per frame. See residual_ms:
            # this is what says whether a burst made the renderer's work slower or made it wait.
            "residual_ms": summarize_ms(residual_ms(b["frame_ms"], b["gpu_ms"])),
            "gpu_mem_used_mb": summarize_ms(b["mem_mb"]),
            "worst_1pct_frame_ms": worst_percent_mean(b["frame_ms"]),
            # The two halves of the wait-vs-slowdown test: catch-up frames (a blocked frame's queued
            # successor returning immediately) and throughput (which a wait+drain preserves).
            "short_frames_pct": short_frame_pct(b["frame_ms"], idle_median_ms),
            "drain_after_hitch_pct": drain_after_hitch_pct(b["frame_ms"], hitch_ms, idle_median_ms),
            "fps": frames_per_second(b["frame_ms"]),
            # Rate AND count. The rate makes phases of different lengths comparable; the count keeps
            # the reader from over-reading a rate extrapolated from a short window (7 hitches in a
            # 25 s tail is "17/min", which reads far more alarming than it is).
            "hitches": (sum(1 for v in b["frame_ms"] if v > hitch_ms) if hitch_ms else None),
            "stalls": sum(1 for v in b["frame_ms"] if v > STALL_MS),
            "seconds": round(sum(b["frame_ms"]) / 1000.0, 1),
            "hitches_per_min": (hitch_rate_per_min(b["frame_ms"], hitch_ms) if hitch_ms else None),
            "stalls_per_min": hitch_rate_per_min(b["frame_ms"], STALL_MS),
        }

    base = per_phase.get("idle")
    burst = per_phase.get("burst")
    delta = {}
    if base and burst:
        delta = {
            "frame_p95_ms": round(burst["frame_ms"]["p95"] - base["frame_ms"]["p95"], 3),
            "frame_p99_ms": round(burst["frame_ms"]["p99"] - base["frame_ms"]["p99"], 3),
            "frame_mean_ms": round(burst["frame_ms"]["mean"] - base["frame_ms"]["mean"], 3),
            "gpu_mean_ms": round(burst["gpu_ms"]["mean"] - base["gpu_ms"]["mean"], 3),
            # The frame delta split into the renderer's own GPU execution and everything else
            # (#1025). Reported at the same three statistics as the frame delta so the split can be
            # read where the cost actually lives — on a client with vsync slack the mean absorbs it
            # and only the tail moves.
            "gpu_p95_ms": round(burst["gpu_ms"]["p95"] - base["gpu_ms"]["p95"], 3),
            "gpu_p99_ms": round(burst["gpu_ms"]["p99"] - base["gpu_ms"]["p99"], 3),
            "residual_mean_ms": round(burst["residual_ms"]["mean"] - base["residual_ms"]["mean"], 3),
            "residual_p95_ms": round(burst["residual_ms"]["p95"] - base["residual_ms"]["p95"], 3),
            "residual_p99_ms": round(burst["residual_ms"]["p99"] - base["residual_ms"]["p99"], 3),
            "short_frames_pct": (
                round(burst["short_frames_pct"] - base["short_frames_pct"], 2)
                if burst["short_frames_pct"] is not None and base["short_frames_pct"] is not None
                else None
            ),
            "fps_ratio": (round(burst["fps"] / base["fps"], 3) if base["fps"] else None),
            "worst_1pct_ms": round(burst["worst_1pct_frame_ms"] - base["worst_1pct_frame_ms"], 3),
            "hitches_per_min": (
                round(burst["hitches_per_min"] - base["hitches_per_min"], 2)
                if burst["hitches_per_min"] is not None and base["hitches_per_min"] is not None
                else None
            ),
            # Ratio as well as difference: a +4 ms p99 means something very different on a 6 ms
            # baseline than on a 30 ms one.
            "frame_p99_ratio": (
                round(burst["frame_ms"]["p99"] / base["frame_ms"]["p99"], 3) if base["frame_ms"]["p99"] else None
            ),
        }

    budget_mb = frame_report.get("gpu_mem_budget_mb", 0.0)
    peak_used = max((p["gpu_mem_used_mb"]["max"] for p in per_phase.values()), default=0.0)
    vram = {
        "budget_mb": budget_mb,
        "game_peak_used_mb": peak_used,
        "game_baseline_used_mb": base["gpu_mem_used_mb"]["mean"] if base else 0.0,
        # Headroom is what remains after the GAME's device-local usage. On Ollama the model's own
        # footprint is reported separately (model_vram_mb) because it lives in another process and
        # the game's VK_EXT_memory_budget view cannot see it — though the driver does shrink the
        # BUDGET it offers the game in response, which is where a resident model shows up here.
        "headroom_mb": round(budget_mb - peak_used, 1) if budget_mb else 0.0,
        "model_vram_mb": model_vram_mb(driver),
    }

    return {
        # 2 (#1022): the hitch counter became relative to the idle baseline, so a v1 report's
        # hitch numbers mean something different and must not be averaged with a v2's. aggregate.py
        # treats this as a cell-identity field for exactly that reason.
        "schema_version": 2,
        "gpu_info": frame_report.get("gpu_info", ""),
        # In observer mode the client joined a server that owns the mission, so the game has no
        # scene name of its own to record; the runner passes what it launched the server with.
        "scene": frame_report.get("scene") or scene,
        "os": f"{platform.system()} {platform.release()}",
        # The cell label (e.g. "cuda"/"vulkan") the runner was invoked with. It is the ONLY thing
        # that distinguishes two backends on the same box+model (endpoint aside), so aggregate.py
        # keys on it to refuse mixing runs from different cells into one distribution (#1016).
        "label": label,
        "endpoint": driver.get("endpoint", ""),
        "model": driver.get("model", ""),
        "workload": driver.get("workload", ""),
        "concurrency": driver.get("concurrency", 1),
        "model_load_probe_s": driver.get("model_load_probe_s"),
        "frames_total": frame_report.get("frames", 0),
        # Frames outside every phase window: the run's bracketing margin, not an error. See
        # sanity_warnings.
        "frames_outside_schedule": outside,
        "settle_s": settle_s,
        # The client's operating point. Recorded because a vsync-limited client settles on an
        # integer divisor of the refresh interval, and two runs of the "same" cell that landed on
        # different divisors are two populations rather than one noisy one (#1019). It is also the
        # calibration input for hitch_threshold_ms, so both live next to each other in the report.
        "idle_frame_interval_median_ms": idle_median_ms,
        "hitch_threshold_ms": hitch_ms,
        "requests": driver.get("total_requests", 0),
        "request_errors": driver.get("total_errors", 0),
        "per_phase": per_phase,
        "delta_burst_vs_idle": delta,
        "vram": vram,
        "warnings": sanity_warnings(frame_report, driver, buckets, samples, phases, settle_s),
    }


def render_markdown(report):
    """Markdown in the style of docs/ai-provider-evaluation.md, ready to paste into the results table."""
    lines = []
    lines.append(f"### GPU contention — {report['os']}")
    lines.append("")
    lines.append(f"- GPU: `{report['gpu_info'] or 'unknown'}`  scene: `{report['scene'] or 'n/a'}`")
    lines.append(
        f"- Model: `{report['model']}` via `{report['endpoint']}` "
        f"(workload `{report['workload']}`, concurrency {report['concurrency']})"
    )
    lines.append(
        f"- {report['frames_total']} frames, {report['requests']} requests, "
        f"{report['request_errors']} errors, warm-up {report['model_load_probe_s']} s"
    )
    lines.append("")
    hitch_ms = report.get("hitch_threshold_ms")
    idle_median = report.get("idle_frame_interval_median_ms")
    if hitch_ms:
        lines.append(
            f"- Idle frame interval (median) `{idle_median:.2f} ms` — hitch threshold "
            f"`>{hitch_ms:.2f} ms` ({HITCH_IDLE_MULTIPLE:g}x the baseline), stall `>{STALL_MS:.0f} ms`"
        )
    else:
        lines.append("- Hitch threshold unavailable: no idle baseline to calibrate against")
    lines.append("")
    lines.append("| Phase | Frames | Secs | Frame mean | Frame p95 | Frame p99 | Worst 1% | GPU mean | Hitches |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for kind in ("idle", "burst", "gap", "tail"):
        p = report["per_phase"].get(kind)
        if not p:
            continue
        f, g = p["frame_ms"], p["gpu_ms"]
        hitches = (
            f"{p['hitches']} ({p['hitches_per_min']:.1f}/min)" if p["hitches"] is not None else "n/a"
        )
        lines.append(
            f"| {kind} | {f['n']} | {p['seconds']:.0f} | {f['mean']:.2f} ms | {f['p95']:.2f} ms | "
            f"{f['p99']:.2f} ms | {p['worst_1pct_frame_ms']:.2f} ms | {g['mean']:.2f} ms | "
            f"{hitches} |"
        )
    lines.append("")

    d = report["delta_burst_vs_idle"]
    if d:
        ratio = f"{d['frame_p99_ratio']:.2f}x" if d.get("frame_p99_ratio") else "n/a"
        lines.append("**Burst vs idle baseline**")
        lines.append("")
        lines.append(
            f"- Frame p99 {d['frame_p99_ms']:+.2f} ms ({ratio}), p95 {d['frame_p95_ms']:+.2f} ms, "
            f"mean {d['frame_mean_ms']:+.2f} ms"
        )
        lines.append(f"- Worst 1% {d['worst_1pct_ms']:+.2f} ms, GPU mean {d['gpu_mean_ms']:+.2f} ms")
        hitch_delta = (
            f"{d['hitches_per_min']:+.2f}" if d.get("hitches_per_min") is not None else "n/a"
        )
        lines.append(f"- Hitches/min {hitch_delta}")
        lines.append("")

        # Where the delta lives. Emitted only when the device produced real timestamps — without
        # them gpu_ms is 0, the "split" is definitionally frame == residual, and printing it would
        # dress a missing measurement up as a finding (the warnings block says so separately).
        burst_phase = report["per_phase"].get("burst", {})
        if burst_phase.get("gpu_ms", {}).get("max", 0) > 0:
            lines.append("**Where the delta lives** — frame = renderer GPU execution + everything else")
            lines.append("")
            lines.append("| | Δ frame | Δ renderer-GPU | Δ residual |")
            lines.append("|---|---:|---:|---:|")
            for q, gkey, rkey, fkey in (
                ("mean", "gpu_mean_ms", "residual_mean_ms", "frame_mean_ms"),
                ("p95", "gpu_p95_ms", "residual_p95_ms", "frame_p95_ms"),
                ("p99", "gpu_p99_ms", "residual_p99_ms", "frame_p99_ms"),
            ):
                lines.append(f"| {q} | {d[fkey]:+.3f} ms | {d[gkey]:+.3f} ms | {d[rkey]:+.3f} ms |")
            lines.append("")
            lines.append(
                "Residual is per-frame `frame_ms - gpu_ms` — CPU frame work, queue wait and present. "
                "A delta that lands in the GPU column is the renderer's work getting slower; one that "
                "lands in the residual is the renderer waiting. The columns are additive at the mean "
                "only; at p95/p99 each percentile is taken independently."
            )
            lines.append("")
            base_p, burst_p = report["per_phase"].get("idle", {}), burst_phase
            if base_p.get("short_frames_pct") is not None and burst_p.get("short_frames_pct") is not None:
                lines.append(
                    f"- Catch-up frames (<{SHORT_FRAME_FRACTION:g}x the idle cadence): "
                    f"{base_p['short_frames_pct']:.2f}% idle -> {burst_p['short_frames_pct']:.2f}% burst; "
                    f"throughput {base_p['fps']:.2f} -> {burst_p['fps']:.2f} fps "
                    f"({d['fps_ratio']:.3f}x)"
                )
                if burst_p.get("drain_after_hitch_pct") is not None:
                    lines.append(
                        f"- Hitches immediately followed by a catch-up frame: "
                        f"{burst_p['drain_after_hitch_pct']:.1f}% of burst hitches"
                        + (
                            f" (idle {base_p['drain_after_hitch_pct']:.1f}%)"
                            if base_p.get("drain_after_hitch_pct") is not None
                            else ""
                        )
                    )
                lines.append("")

    v = report["vram"]
    model_vram = f"{v['model_vram_mb']:.0f} MB" if v.get("model_vram_mb") else "not reported by endpoint"
    lines.append("**VRAM**")
    lines.append("")
    lines.append(
        f"- Game peak {v['game_peak_used_mb']:.0f} MB of {v['budget_mb']:.0f} MB budget "
        f"(headroom {v['headroom_mb']:.0f} MB); model {model_vram}"
    )
    lines.append("")

    if report["warnings"]:
        lines.append("**Warnings**")
        lines.append("")
        for w in report["warnings"]:
            lines.append(f"- {w}")
        lines.append("")
    return "\n".join(lines)


# ---- main --------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description="Analyze LLM-vs-renderer GPU contention (#782)")
    ap.add_argument("--frame-stats", required=True, help="the game's --frame-stats-json document")
    ap.add_argument("--driver", required=True, help="driver.py's phase report")
    ap.add_argument("--settle-s", type=float, default=SETTLE_S, help="seconds discarded after each phase boundary")
    ap.add_argument("--scene", default="", help="scene label when the client joined a server that owns the mission")
    ap.add_argument("--label", default="", help="cell label (e.g. cuda/vulkan); recorded so aggregate.py can group runs")
    ap.add_argument("--out", default="", help="output path prefix (writes <prefix>.json and <prefix>.md)")
    args = ap.parse_args(argv)

    frame_report = json.loads(Path(args.frame_stats).read_text(encoding="utf-8"))
    driver = json.loads(Path(args.driver).read_text(encoding="utf-8"))
    report = contention_metrics(frame_report, driver, args.settle_s, args.scene, args.label)
    md = render_markdown(report)
    print(md)

    prefix = args.out or str(
        Path(__file__).resolve().parent
        / "results"
        / f"{platform.system().lower()}_{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
    )
    out = Path(prefix)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    out.with_suffix(".md").write_text(md + "\n", encoding="utf-8")
    print(f"[gpu-contention] wrote {out.with_suffix('.json')} and {out.with_suffix('.md')}", file=sys.stderr)
    # A run whose numbers are not trustworthy must not exit 0 — the per-OS runners and any future
    # automation key off this.
    return 1 if report["warnings"] else 0


if __name__ == "__main__":
    sys.exit(main())
