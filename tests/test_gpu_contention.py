# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/gpu_contention/{driver,analyze}.py (#782).

Pure-logic coverage only — no network, no model, no GPU, no game binary. Per the initiative's CI
policy (docs/ai-architecture.md §7) CI must never require a model, so what is exercised here is
the schedule arithmetic and the frame/phase join + summary layer; the HTTP edge and the subprocess
orchestration are not touched.
"""

import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load(name):
    path = REPO_ROOT / "tools" / "gpu_contention" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"gpu_contention_{name}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


drv = _load("driver")
an = _load("analyze")


# ---- build_schedule ----------------------------------------------------------------------------


def test_schedule_alternates_bursts_and_gaps_with_no_trailing_gap():
    phases = drv.build_schedule(0.0, idle_seconds=10, bursts=3, burst_seconds=5, gap_seconds=5, tail_seconds=10)
    kinds = [p["phase"] for p in phases]
    # The gap after the LAST burst is dropped — the tail already measures post-burst idle.
    assert kinds == ["idle", "burst", "gap", "burst", "gap", "burst", "tail"]
    assert drv.schedule_duration_s(phases) == 45.0


def test_schedule_windows_are_contiguous_and_ordered():
    phases = drv.build_schedule(1000.0, 10, 2, 5, 5, 10)
    assert phases[0]["start_ms"] == 1000.0
    for a, b in zip(phases, phases[1:]):
        assert a["end_ms"] == b["start_ms"]
        assert a["start_ms"] < a["end_ms"]


def test_sustained_mode_replaces_the_burst_train():
    phases = drv.build_schedule(0.0, 10, 5, 20, 20, 10, sustained_seconds=120)
    kinds = [p["phase"] for p in phases]
    assert kinds == ["idle", "burst", "tail"]
    assert phases[1]["end_ms"] - phases[1]["start_ms"] == 120_000.0


def test_zero_length_phases_are_omitted_not_emitted_empty():
    phases = drv.build_schedule(0.0, idle_seconds=0, bursts=1, burst_seconds=5, gap_seconds=0, tail_seconds=0)
    assert [p["phase"] for p in phases] == ["burst"]
    assert drv.schedule_duration_s([]) == 0.0


def test_zero_bursts_yields_only_idle_phases():
    phases = drv.build_schedule(0.0, idle_seconds=10, bursts=0, burst_seconds=5, gap_seconds=5, tail_seconds=10)
    assert [p["phase"] for p in phases] == ["idle", "tail"]


# ---- summarize_ms / worst_percent_mean -----------------------------------------------------------


def test_summarize_ms_empty_is_zeroed():
    s = an.summarize_ms([])
    assert s["n"] == 0 and s["p99"] == 0.0 and s["max"] == 0.0


def test_summarize_ms_nearest_rank_percentiles():
    s = an.summarize_ms(list(range(1, 101)))  # 1..100
    assert s["n"] == 100
    assert s["min"] == 1.0 and s["max"] == 100.0
    assert s["mean"] == 50.5
    # Nearest-rank over 0-based indices: p95 -> index int(0.95*99) = 94 -> value 95.
    assert s["p95"] == 95.0
    assert s["p99"] == 99.0


def test_worst_percent_mean_takes_the_slow_tail():
    values = [10.0] * 99 + [500.0]
    assert an.worst_percent_mean(values, 0.01) == 500.0
    assert an.worst_percent_mean([]) == 0.0


# ---- classify_samples ----------------------------------------------------------------------------


def _phases():
    # idle 0-10 s, burst 10-20 s, tail 20-30 s, in epoch ms.
    return [
        {"phase": "idle", "index": 0, "start_ms": 0.0, "end_ms": 10_000.0},
        {"phase": "burst", "index": 0, "start_ms": 10_000.0, "end_ms": 20_000.0},
        {"phase": "tail", "index": 0, "start_ms": 20_000.0, "end_ms": 30_000.0},
    ]


def test_classify_settle_period_is_excluded_from_each_phase():
    # One sample 1 s into the burst (inside the settle window) and one 7 s in (outside it).
    samples = [[11_000.0, 20.0, 18.0, 100.0], [17_000.0, 25.0, 22.0, 110.0]]
    buckets, outside = an.classify_samples(samples, _phases(), settle_s=5.0)
    assert outside == 1  # the settling frame belongs to neither side
    assert buckets["burst"]["frame_ms"] == [25.0]


def test_classify_boundary_sample_belongs_to_the_phase_that_started():
    # Exactly at start+settle is IN; exactly at end_ms is OUT (half-open window).
    samples = [[15_000.0, 12.0, 10.0, 50.0], [20_000.0, 13.0, 11.0, 50.0]]
    buckets, outside = an.classify_samples(samples, _phases(), settle_s=5.0)
    assert buckets["burst"]["frame_ms"] == [12.0]
    # 20_000 is the tail's start, but inside the tail's settle window -> outside.
    assert outside == 1


def test_classify_orphan_samples_outside_all_windows_are_counted():
    samples = [[999_999.0, 9.0, 8.0, 10.0]]
    buckets, outside = an.classify_samples(samples, _phases())
    assert buckets == {}
    assert outside == 1


def test_classify_ignores_malformed_short_rows():
    samples = [[100.0, 9.0]]  # truncated row
    buckets, outside = an.classify_samples(samples, _phases())
    assert buckets == {} and outside == 0


def test_classify_tracks_per_window_counts_for_the_coverage_check():
    phases = [
        {"phase": "burst", "index": 0, "start_ms": 0.0, "end_ms": 10_000.0},
        {"phase": "burst", "index": 1, "start_ms": 10_000.0, "end_ms": 20_000.0},
    ]
    # Frames only in the first burst window.
    samples = [[float(t), 10.0, 8.0, 100.0] for t in range(6000, 10_000, 500)]
    buckets, _ = an.classify_samples(samples, phases, settle_s=5.0)
    assert set(buckets["burst"]["windows"]) == {0}


# ---- hitch_rate_per_min --------------------------------------------------------------------------


def test_hitch_rate_is_per_minute_of_elapsed_frame_time():
    # 60 frames of 1000 ms = 60 s of wall time; 6 of them are over the threshold.
    values = [1000.0] * 54 + [2000.0] * 6
    rate = an.hitch_rate_per_min(values, 1500.0)
    total_min = sum(values) / 1000.0 / 60.0
    assert rate == round(6 / total_min, 2)
    assert an.hitch_rate_per_min([], 33.3) == 0.0


# ---- contention_metrics / render_markdown ---------------------------------------------------------


def _frame_report(samples, budget_mb=16000.0):
    return {
        "schema_version": 1,
        "frames": len(samples),
        "gpu_info": "Test GPU",
        "scene": "builtin:sandbox",
        "gpu_mem_budget_mb": budget_mb,
        "gpu_ms": {"max": 20.0},
        "samples_columns": ["t_ms", "frame_ms", "gpu_ms", "gpu_mem_used_mb"],
        "samples": samples,
    }


def _driver_report(phases, **kw):
    base = {
        "schema_version": 1,
        "endpoint": "http://localhost:11434",
        "model": "qwen2.5-coder:14b",
        "workload": "intent",
        "concurrency": 1,
        "model_load_probe_s": 1.2,
        "total_requests": 40,
        "total_errors": 0,
        "phases": phases,
    }
    base.update(kw)
    return base


def _synthetic_run(idle_ms=8.0, burst_ms=24.0):
    """A clean run: steady idle frames, slower burst frames, back to idle in the tail."""
    samples = []
    for t in range(6000, 10_000, 100):  # idle, past the settle window
        samples.append([float(t), idle_ms, idle_ms - 2, 3000.0])
    for t in range(16_000, 20_000, 100):  # burst, past the settle window
        samples.append([float(t), burst_ms, burst_ms - 2, 3200.0])
    for t in range(26_000, 30_000, 100):  # tail
        samples.append([float(t), idle_ms, idle_ms - 2, 3000.0])
    return samples


def test_contention_metrics_reports_the_burst_vs_idle_delta():
    report = an.contention_metrics(_frame_report(_synthetic_run()), _driver_report(_phases()))
    assert report["per_phase"]["idle"]["frame_ms"]["mean"] == 8.0
    assert report["per_phase"]["burst"]["frame_ms"]["mean"] == 24.0
    d = report["delta_burst_vs_idle"]
    assert d["frame_mean_ms"] == 16.0
    assert d["frame_p99_ms"] == 16.0
    assert d["frame_p99_ratio"] == 3.0
    assert report["warnings"] == []


def test_contention_metrics_vram_headroom_and_model_footprint():
    driver = _driver_report(
        _phases(),
        loaded_models_after=[
            {"name": "qwen2.5-coder:14b", "size_bytes": 9_000_000_000, "size_vram_bytes": 9_437_184_000}
        ],
    )
    report = an.contention_metrics(_frame_report(_synthetic_run(), budget_mb=16000.0), driver)
    v = report["vram"]
    assert v["game_peak_used_mb"] == 3200.0
    assert v["headroom_mb"] == 12800.0
    assert v["model_vram_mb"] == 9000.0  # 9_437_184_000 bytes exactly


def test_model_vram_falls_back_to_any_loaded_model_then_none():
    assert an.model_vram_mb({"model": "absent", "loaded_models_after": []}) is None
    other = {"model": "absent", "loaded_models_after": [{"name": "other", "size_vram_bytes": 1024 * 1024 * 100}]}
    assert an.model_vram_mb(other) == 100.0


def test_warnings_fire_when_the_run_did_not_overlap():
    # Frames only during idle: the game exited before the burst, so there is nothing to compare.
    samples = [[float(t), 8.0, 6.0, 3000.0] for t in range(6000, 10_000, 100)]
    report = an.contention_metrics(_frame_report(samples), _driver_report(_phases()))
    assert report["delta_burst_vs_idle"] == {}
    assert any("burst window" in w for w in report["warnings"])


def test_frames_outside_the_schedule_are_reported_but_not_warned_about():
    # The runners deliberately start the game early and let it outlive the schedule, so most frames
    # in a short schedule fall outside it. That is the design, not a fault — warning on it would
    # fire on every correct run.
    samples = [[float(t), 8.0, 6.0, 3000.0] for t in range(-60_000, 90_000, 100)]
    samples += _synthetic_run()
    samples.sort(key=lambda r: r[0])
    report = an.contention_metrics(_frame_report(samples), _driver_report(_phases()))
    assert report["frames_outside_schedule"] > 0
    assert report["warnings"] == []


def test_warning_fires_when_recording_stopped_before_the_final_phase():
    # Frames through the burst but none in the tail: the client died before the recovery window.
    samples = [[float(t), 8.0, 6.0, 3000.0] for t in range(6000, 10_000, 100)]
    samples += [[float(t), 24.0, 22.0, 3200.0] for t in range(16_000, 20_000, 100)]
    report = an.contention_metrics(_frame_report(samples), _driver_report(_phases()))
    assert any("before the final" in w for w in report["warnings"])


def test_warning_fires_when_a_middle_burst_window_recorded_nothing():
    # Two bursts, frames only in the first: the client stopped rendering part-way through, and the
    # surviving window would otherwise stand in for the whole run.
    phases = [
        {"phase": "idle", "index": 0, "start_ms": 0.0, "end_ms": 10_000.0},
        {"phase": "burst", "index": 0, "start_ms": 10_000.0, "end_ms": 20_000.0},
        {"phase": "burst", "index": 1, "start_ms": 20_000.0, "end_ms": 30_000.0},
    ]
    samples = [[float(t), 8.0, 6.0, 3000.0] for t in range(6000, 10_000, 100)]
    samples += [[float(t), 20.0, 18.0, 3200.0] for t in range(16_000, 20_000, 100)]
    report = an.contention_metrics(_frame_report(samples), _driver_report(phases))
    assert any("burst window(s) [1]" in w for w in report["warnings"])


def test_scene_override_fills_in_when_the_client_had_none():
    # Observer mode joins a server that owns the mission, so the game records no scene of its own.
    fr = _frame_report(_synthetic_run())
    fr["scene"] = ""
    report = an.contention_metrics(fr, _driver_report(_phases()), scene="builtin:sandbox")
    assert report["scene"] == "builtin:sandbox"


def test_warnings_fire_on_request_errors_and_missing_gpu_timestamps():
    fr = _frame_report(_synthetic_run())
    fr["gpu_ms"] = {"max": 0.0}
    report = an.contention_metrics(fr, _driver_report(_phases(), total_errors=3))
    joined = " ".join(report["warnings"])
    assert "request errors" in joined
    assert "timestamp queries" in joined


def test_render_markdown_has_a_row_per_phase_and_the_delta_block():
    report = an.contention_metrics(_frame_report(_synthetic_run()), _driver_report(_phases()))
    md = an.render_markdown(report)
    assert "| Phase | Frames |" in md
    assert "| idle |" in md and "| burst |" in md and "| tail |" in md
    assert "Burst vs idle baseline" in md
    assert "**VRAM**" in md
    # No warnings section when the run was clean.
    assert "**Warnings**" not in md


def test_render_markdown_surfaces_warnings_when_present():
    samples = [[float(t), 8.0, 6.0, 3000.0] for t in range(6000, 10_000, 100)]
    report = an.contention_metrics(_frame_report(samples), _driver_report(_phases()))
    assert "**Warnings**" in an.render_markdown(report)
