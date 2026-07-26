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
agg = _load("aggregate")


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


# ---- aggregate.py (repeat-and-aggregate, #1016) --------------------------------------------------


def _agg_report(
    ratio,
    p99,
    mean,
    *,
    label="vulkan",
    model="qwen2.5-coder:14b",
    gpu="RTX 5080",
    warnings=None,
    schema_version=2,
    idle_interval_ms=4.6,
    with_split=False,
):
    """A minimal analyze.py-shaped report carrying just what aggregate.py reads.

    `with_split` adds the #1025 wait-vs-slowdown fields. They were added additively without a
    schema bump, so a set can legitimately mix reports that have them with reports that do not —
    which is exactly what coverage_warnings exists to surface.
    """
    delta = {
        "frame_p99_ratio": ratio,
        "frame_p99_ms": p99,
        "frame_p95_ms": round(p99 * 0.8, 3),
        "frame_mean_ms": mean,
        "worst_1pct_ms": round(p99 * 2, 3),
        "gpu_mean_ms": 0.7,
        "hitches_per_min": 10.0,
    }
    if with_split:
        delta.update(
            {
                "gpu_p95_ms": 0.03,
                "gpu_p99_ms": 0.18,
                "residual_p95_ms": 4.27,
                "residual_p99_ms": 7.10,
                "residual_mean_ms": 0.03,
                "short_frames_pct": 15.1,
                "fps_ratio": 0.99,
            }
        )
    return {
        "schema_version": schema_version,
        "os": "Windows 10.0.26100",
        "gpu_info": gpu,
        "model": model,
        "label": label,
        "idle_frame_interval_median_ms": idle_interval_ms,
        "endpoint": "http://localhost:8080",
        "workload": "intent",
        "scene": "builtin:sandbox",
        "delta_burst_vs_idle": delta,
        "vram": {"model_vram_mb": 9031.0, "budget_mb": 4556.0},
        "warnings": warnings or [],
    }


def test_across_run_stats_reports_median_and_range():
    s = agg.across_run_stats([1.49, 2.48, 2.43, 1.85, 2.10])
    assert s["n"] == 5
    assert s["min"] == 1.49 and s["max"] == 2.48
    assert s["median"] == 2.10
    assert s["spread"] == round(2.48 - 1.49, 4)


def test_across_run_stats_drops_none_and_handles_empty():
    assert agg.across_run_stats([]) == {"median": None, "min": None, "max": None, "mean": None, "spread": None, "n": 0}
    s = agg.across_run_stats([3.0, None, 5.0])
    assert s["n"] == 2 and s["min"] == 3.0 and s["max"] == 5.0


def test_aggregate_folds_runs_into_a_distribution():
    reports = [_agg_report(r, p, 0.05) for r, p in [(1.49, 2.24), (2.48, 6.78), (2.43, 6.64), (1.85, 3.9), (2.10, 4.5)]]
    a = agg.aggregate_reports(reports)
    assert a["runs"] == 5
    ratio = a["delta_burst_vs_idle"]["frame_p99_ratio"]
    assert ratio["min"] == 1.49 and ratio["max"] == 2.48
    # The whole #1016 point: mean is stable while p99 wanders.
    assert a["delta_burst_vs_idle"]["frame_mean_ms"]["spread"] < 0.1
    assert a["warnings"] == []


def test_p99_verdict_flags_noise_when_spread_exceeds_effect():
    # Spread 0.99x on a median effect of ~1.1x above baseline -> "wide" (quote the range).
    reports = [_agg_report(r, p, 0.05) for r, p in [(1.49, 2.24), (2.48, 6.78), (2.43, 6.64), (1.85, 3.9), (2.10, 4.5)]]
    v = agg.aggregate_reports(reports)["p99_verdict"]
    assert v["level"] in ("wide", "noise")


def test_p99_verdict_calls_it_noise_when_spread_dominates():
    # Median 1.5x (effect 0.5x), spread 1.4x -> spread >> effect.
    reports = [_agg_report(r, 5.0, 0.05) for r in [1.1, 1.5, 2.5]]
    v = agg.aggregate_reports(reports)["p99_verdict"]
    assert v["level"] == "noise"
    assert "not a measurement" in v["text"]


def test_p99_verdict_calls_it_stable_when_runs_agree():
    reports = [_agg_report(r, 6.0, 0.05) for r in [2.00, 2.02, 1.99, 2.01]]
    v = agg.aggregate_reports(reports)["p99_verdict"]
    assert v["level"] == "stable"


def test_p99_verdict_needs_at_least_two_runs():
    v = agg.aggregate_reports([_agg_report(1.5, 3.0, 0.05)])["p99_verdict"]
    assert v["level"] == "insufficient"


def test_aggregate_refuses_to_blend_different_cells():
    mixed = [_agg_report(1.5, 2.2, 0.05, label="cuda"), _agg_report(2.4, 6.6, 0.05, label="vulkan")]
    a = agg.aggregate_reports(mixed)
    assert any("different cells" in w for w in a["warnings"])


def test_aggregate_flags_a_mismatched_gpu_or_model():
    mixed = [_agg_report(1.5, 2.2, 0.05, gpu="RTX 5080"), _agg_report(1.6, 2.3, 0.05, gpu="RTX 4090")]
    assert any("gpu_info" in w for w in agg.aggregate_reports(mixed)["warnings"])


def test_aggregate_surfaces_dirty_input_runs():
    reports = [_agg_report(2.0, 6.0, 0.05), _agg_report(2.1, 6.2, 0.05, warnings=["client stopped rendering"])]
    a = agg.aggregate_reports(reports)
    assert any("carried their own warnings" in w for w in a["warnings"])


def test_render_aggregate_markdown_shows_ranges_and_verdict():
    reports = [_agg_report(r, p, 0.05) for r, p in [(1.49, 2.24), (2.48, 6.78), (2.10, 4.5)]]
    md = agg.render_aggregate_markdown(agg.aggregate_reports(reports))
    assert "aggregate, 3 runs" in md
    assert "[1.49–2.48]" in md  # the min–max range is shown, not just a point
    assert "p99 stability:" in md


def test_aggregate_empty_raises():
    import pytest

    with pytest.raises(ValueError):
        agg.aggregate_reports([])


# ---- relative hitch threshold (#1022) ------------------------------------------------------------


def test_hitch_threshold_is_a_multiple_of_the_idle_baseline():
    assert an.hitch_threshold_ms(8.0) == 16.0
    assert an.hitch_threshold_ms(33.33) == round(33.33 * 2, 3)


def test_hitch_threshold_is_unavailable_without_an_idle_baseline():
    # None rather than a fixed fallback: "could not measure" is not "measured zero".
    assert an.hitch_threshold_ms(None) is None
    assert an.hitch_threshold_ms(0.0) is None
    assert an.median_frame_ms([]) is None


def test_idle_frames_are_not_hitches_on_a_slow_client():
    """The #1022 defect: a 33.3 ms idle cadence scored every ordinary frame as a hitch.

    Under the old fixed >33.3 ms threshold this run's idle phase counted ~all of its frames. Against
    its own baseline it counts none, and the burst's genuinely-doubled frames still do.
    """
    report = an.contention_metrics(
        _frame_report(_synthetic_run(idle_ms=33.3, burst_ms=70.0)), _driver_report(_phases())
    )
    assert report["idle_frame_interval_median_ms"] == 33.3
    assert report["hitch_threshold_ms"] == 66.6
    assert report["per_phase"]["idle"]["hitches"] == 0
    assert report["per_phase"]["burst"]["hitches"] == report["per_phase"]["burst"]["frame_ms"]["n"]
    assert report["delta_burst_vs_idle"]["hitches_per_min"] > 0


def test_hitch_threshold_scales_with_the_client_so_fast_and_slow_runs_agree():
    """The same relative slowdown reads the same on a 240 Hz client and a 30 fps one."""
    fast = an.contention_metrics(
        _frame_report(_synthetic_run(idle_ms=4.0, burst_ms=12.0)), _driver_report(_phases())
    )
    slow = an.contention_metrics(
        _frame_report(_synthetic_run(idle_ms=40.0, burst_ms=120.0)), _driver_report(_phases())
    )
    for r in (fast, slow):
        assert r["per_phase"]["idle"]["hitches"] == 0
        assert r["per_phase"]["burst"]["hitches"] == r["per_phase"]["burst"]["frame_ms"]["n"]


def test_hitch_metrics_report_unavailable_when_there_is_no_idle_phase():
    phases = [{"phase": "burst", "index": 0, "start_ms": 0.0, "end_ms": 20_000.0}]
    samples = [[float(t), 20.0, 18.0, 3000.0] for t in range(6000, 20_000, 100)]
    report = an.contention_metrics(_frame_report(samples), _driver_report(phases))
    assert report["hitch_threshold_ms"] is None
    assert report["per_phase"]["burst"]["hitches"] is None
    assert report["per_phase"]["burst"]["hitches_per_min"] is None
    # The stall counter is absolute and keeps working — it is a perception band, not a frame-rate one.
    assert report["per_phase"]["burst"]["stalls"] == 0
    md = an.render_markdown(report)
    assert "n/a" in md and "Hitch threshold unavailable" in md


def test_markdown_states_the_calibrated_threshold():
    report = an.contention_metrics(_frame_report(_synthetic_run()), _driver_report(_phases()))
    md = an.render_markdown(report)
    assert "Idle frame interval (median)" in md
    assert "hitch threshold" in md


def test_report_schema_version_is_2_so_old_reports_are_not_blended():
    report = an.contention_metrics(_frame_report(_synthetic_run()), _driver_report(_phases()))
    assert report["schema_version"] == 2


def test_aggregate_refuses_to_blend_reports_from_different_schema_versions():
    reports = [_agg_report(2.0, 6.0, 0.05), _agg_report(2.1, 6.2, 0.05, schema_version=1)]
    a = agg.aggregate_reports(reports)
    assert any("schema_version" in w for w in a["warnings"])


def test_aggregate_flags_runs_that_sat_on_different_vsync_divisors():
    """The #1019 cell: one run at 15 fps, four at 30 fps, every declared field identical."""
    reports = [_agg_report(1.6, 21.0, 1.9, idle_interval_ms=33.33) for _ in range(4)]
    reports.append(_agg_report(0.88, -12.67, 4.38, idle_interval_ms=66.08))
    a = agg.aggregate_reports(reports)
    assert any("different frame cadences" in w for w in a["warnings"])


def test_aggregate_tolerates_ordinary_cadence_jitter():
    reports = [_agg_report(2.0, 6.0, 0.05, idle_interval_ms=ms) for ms in (4.58, 4.60, 4.65, 4.72)]
    a = agg.aggregate_reports(reports)
    assert a["warnings"] == []
    assert a["run_metrics"]["idle_frame_interval_median_ms"]["n"] == 4


def test_aggregate_markdown_reports_the_idle_frame_interval():
    reports = [_agg_report(2.0, 6.0, 0.05, idle_interval_ms=4.6) for _ in range(3)]
    md = agg.render_aggregate_markdown(agg.aggregate_reports(reports))
    assert "Idle frame interval" in md


# ---- the wait-vs-slowdown split (#1025) ----------------------------------------------------------
#
# Frame time alone cannot tell "the renderer's GPU work got slower" from "the renderer waited to be
# scheduled" — the two look identical in the frame column and want opposite fixes. Splitting each
# frame into the renderer's own GPU span and the remainder separates them, which is what resolved
# #1025 from artifacts already on disk rather than from a two-day tracing session.


def _split_run(idle_frame, idle_gpu, burst_frame, burst_gpu):
    """A run with frame time and GPU span varied INDEPENDENTLY, which _synthetic_run cannot do."""
    samples = []
    for t in range(6000, 10_000, 100):
        samples.append([float(t), idle_frame, idle_gpu, 3000.0])
    for t in range(16_000, 20_000, 100):
        samples.append([float(t), burst_frame, burst_gpu, 3200.0])
    for t in range(26_000, 30_000, 100):
        samples.append([float(t), idle_frame, idle_gpu, 3000.0])
    return samples


def test_residual_is_frame_minus_gpu_per_frame():
    assert an.residual_ms([10.0, 20.0], [4.0, 5.0]) == [6.0, 15.0]
    assert an.residual_ms([], []) == []
    # Ragged input pairs positionally and ignores the surplus rather than raising.
    assert an.residual_ms([10.0, 20.0, 30.0], [4.0]) == [6.0]


def test_short_frame_pct_counts_the_catch_up_drain():
    # Half the frames come back in well under half the 8 ms cadence.
    frames = [8.0, 8.0, 0.5, 0.5]
    assert an.short_frame_pct(frames, 8.0) == 50.0
    assert an.short_frame_pct([8.0, 8.0], 8.0) == 0.0


def test_short_frame_pct_is_unavailable_without_an_idle_baseline():
    # Same principle as the hitch threshold: "could not measure" is not "measured zero".
    assert an.short_frame_pct([8.0, 0.1], None) is None
    assert an.short_frame_pct([8.0, 0.1], 0.0) is None
    assert an.short_frame_pct([], 8.0) is None


def test_drain_after_hitch_conditions_short_frames_on_the_hitch_before_them():
    # 8 ms cadence, hitch >16 ms, catch-up <4 ms. Two hitches; only the first is drained.
    frames = [8.0, 20.0, 1.0, 8.0, 20.0, 8.0]
    assert an.drain_after_hitch_pct(frames, 16.0, 8.0) == 50.0
    # A run with no hitch has nothing to condition on — that is not "0 % drained".
    assert an.drain_after_hitch_pct([8.0] * 10, 16.0, 8.0) is None
    assert an.drain_after_hitch_pct(frames, None, 8.0) is None
    assert an.drain_after_hitch_pct(frames, 16.0, None) is None


def test_drain_after_hitch_ignores_a_trailing_hitch_with_no_successor():
    # The final frame has no next frame, so it cannot be evidence either way.
    assert an.drain_after_hitch_pct([8.0, 8.0, 20.0], 16.0, 8.0) is None


def test_frames_per_second_is_throughput_over_the_frames_it_holds():
    assert an.frames_per_second([10.0] * 100) == 100.0
    assert an.frames_per_second([]) == 0.0


def test_a_wait_lands_in_the_residual_and_not_in_the_gpu_column():
    """Frames doubled, renderer's GPU span untouched — the Windows signature (#1025)."""
    run = _split_run(idle_frame=4.16, idle_gpu=1.25, burst_frame=8.32, burst_gpu=1.25)
    d = an.contention_metrics(_frame_report(run), _driver_report(_phases()))["delta_burst_vs_idle"]
    assert d["frame_mean_ms"] == 4.16
    assert d["gpu_mean_ms"] == 0.0
    assert d["residual_mean_ms"] == 4.16
    assert d["gpu_p99_ms"] == 0.0
    assert d["residual_p99_ms"] == 4.16


def test_a_slowdown_lands_in_the_gpu_column_and_not_in_the_residual():
    """The opposite mechanism — the renderer's own work takes longer (the Linux signature)."""
    run = _split_run(idle_frame=4.16, idle_gpu=1.25, burst_frame=5.16, burst_gpu=2.25)
    d = an.contention_metrics(_frame_report(run), _driver_report(_phases()))["delta_burst_vs_idle"]
    assert d["frame_mean_ms"] == 1.0
    assert d["gpu_mean_ms"] == 1.0
    assert d["residual_mean_ms"] == 0.0


def test_throughput_ratio_separates_a_drain_from_a_real_slowdown():
    # A burst of blocked frames, each followed by the catch-up frame it queued, then an ordinary
    # one. Three frames still take three cadences between them, so the frame RATE does not move —
    # the whole cost is jitter. Idle cadence 8 ms, so hitch >16 ms and catch-up <4 ms.
    pattern = [16.5, 0.5, 7.0]  # sums to 3 x 8.0, so throughput is unchanged by construction
    samples = [[float(t), 8.0, 2.0, 3000.0] for t in range(6000, 10_000, 100)]
    for i, t in enumerate(range(16_000, 19_900, 100)):  # 39 frames = 13 whole patterns
        samples.append([float(t), pattern[i % 3], 2.0, 3200.0])
    samples += [[float(t), 8.0, 2.0, 3000.0] for t in range(26_000, 30_000, 100)]
    report = an.contention_metrics(_frame_report(samples), _driver_report(_phases()))
    d = report["delta_burst_vs_idle"]
    assert report["per_phase"]["burst"]["frame_ms"]["mean"] == 8.0
    assert report["per_phase"]["burst"]["short_frames_pct"] == round(100 / 3, 2)
    assert d["fps_ratio"] == 1.0  # throughput held exactly; the cost is all jitter
    assert d["frame_p99_ms"] == 8.5  # ...and the tail still took a full extra cadence
    # Every hitch here is followed by its own catch-up: one event, not two coincidences.
    assert report["per_phase"]["burst"]["drain_after_hitch_pct"] == 100.0


def test_markdown_shows_where_the_delta_lives():
    run = _split_run(idle_frame=4.16, idle_gpu=1.25, burst_frame=8.32, burst_gpu=1.25)
    md = an.render_markdown(an.contention_metrics(_frame_report(run), _driver_report(_phases())))
    assert "Where the delta lives" in md
    assert "Δ residual" in md
    assert "Catch-up frames" in md


def test_markdown_omits_the_split_when_the_device_reports_no_gpu_timestamps():
    """Without timestamps gpu_ms is 0, so the 'split' is definitionally frame == residual.

    Printing it would dress a missing measurement as a finding; the warnings block says the
    timestamps are absent, and that is the honest output.
    """
    run = _split_run(idle_frame=4.16, idle_gpu=0.0, burst_frame=8.32, burst_gpu=0.0)
    frame_report = _frame_report(run)
    frame_report["gpu_ms"] = {"max": 0.0}
    report = an.contention_metrics(frame_report, _driver_report(_phases()))
    md = an.render_markdown(report)
    assert "Where the delta lives" not in md
    assert any("GPU timestamps read 0" in w for w in report["warnings"])


def test_aggregate_carries_the_split_across_runs():
    reports = []
    for p95 in (4.2, 4.4, 4.5):
        r = _agg_report(2.5, 6.7, 0.04)
        r["delta_burst_vs_idle"].update(
            {"gpu_p95_ms": 0.02, "residual_p95_ms": p95, "residual_p99_ms": p95 + 2, "fps_ratio": 0.99}
        )
        reports.append(r)
    a = agg.aggregate_reports(reports)
    assert a["delta_burst_vs_idle"]["residual_p95_ms"]["median"] == 4.4
    assert a["delta_burst_vs_idle"]["gpu_p95_ms"]["median"] == 0.02
    assert "Residual p95 delta" in agg.render_aggregate_markdown(a)


def test_aggregate_tolerates_reports_predating_the_split():
    """The fields are additive, so a report written before them aggregates without warning."""
    a = agg.aggregate_reports([_agg_report(2.0, 6.0, 0.05) for _ in range(3)])
    assert a["warnings"] == []
    assert a["delta_burst_vs_idle"]["residual_p95_ms"]["median"] is None
    assert "—" in agg.render_aggregate_markdown(a)


# ---- partial-coverage guard (#1025) --------------------------------------------------------------


def test_mixed_report_versions_warn_instead_of_aggregating_silently():
    """A metric only SOME runs carried must not render like one every run carried.

    `across_run_stats` drops the missing entries and `_fmt` prints no `n`, so without this guard a
    row computed from 2 of 5 runs is indistinguishable from a full one — under a header that says
    "aggregate, 5 runs", with a range that looks tight because it is two runs rather than because
    the metric is stable.
    """
    reports = [_agg_report(2.0, 6.0, 0.05, with_split=True) for _ in range(2)]
    reports += [_agg_report(2.0, 6.0, 0.05) for _ in range(3)]
    a = agg.aggregate_reports(reports)
    assert a["runs"] == 5
    assert a["delta_burst_vs_idle"]["residual_p95_ms"]["n"] == 2  # the silent part
    thin = [w for w in a["warnings"] if "fewer runs" in w]
    assert len(thin) == 1
    # Names the metric AND the shortfall, so the reader can act without re-deriving it.
    assert "Residual p95 delta (2/5)" in thin[0]
    assert "Throughput ratio (2/5)" in thin[0]


def test_uniform_report_sets_do_not_warn_either_way():
    """Present in all, or absent from all — both are clean. Only the MIX is a problem."""
    every = agg.aggregate_reports([_agg_report(2.0, 6.0, 0.05, with_split=True) for _ in range(4)])
    none = agg.aggregate_reports([_agg_report(2.0, 6.0, 0.05) for _ in range(4)])
    assert every["warnings"] == []
    assert none["warnings"] == []
    # Absent-from-all stays n == 0 and renders as an em dash rather than being called out.
    assert none["delta_burst_vs_idle"]["residual_p95_ms"]["n"] == 0


def test_a_metric_absent_from_every_report_is_not_flagged():
    """Model VRAM is null on an endpoint with no /api/ps — a real cell, not a mixed input set."""
    reports = [_agg_report(2.0, 6.0, 0.05) for _ in range(3)]
    for r in reports:
        r["vram"]["model_vram_mb"] = None
    a = agg.aggregate_reports(reports)
    assert a["vram"]["model_vram_mb"]["n"] == 0
    assert a["warnings"] == []


def test_the_coverage_warning_reaches_the_rendered_markdown():
    reports = [_agg_report(2.0, 6.0, 0.05, with_split=True)] + [_agg_report(2.0, 6.0, 0.05)] * 3
    md = agg.render_aggregate_markdown(agg.aggregate_reports(reports))
    assert "**Warnings**" in md
    assert "fewer runs" in md
