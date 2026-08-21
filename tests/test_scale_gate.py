# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/bot_swarm/scale_gate.py (issue #520).

Pure-logic coverage only — no sockets, no fl-server/bot_swarm binaries. Mirrors the conventions in
tests/test_gen_terrain_chunks.py / tests/test_latency_compare.py.
"""

import importlib.util
import json
import os
from pathlib import Path

import pytest

# Load scale_gate.py by path (tools/ is not a package).
_MOD_PATH = Path(__file__).resolve().parent.parent / "tools" / "bot_swarm" / "scale_gate.py"
_spec = importlib.util.spec_from_file_location("scale_gate", _MOD_PATH)
sg = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sg)


# ---- load_config / load_profile ----------------------------------------------------------------
def _write_config(tmp_path):
    cfg = {
        "kbs_baseline_tolerance_pct": 10,
        "profiles": {
            "pr": {"_comment": "x", "clients": 64, "duration_s": 30, "patterns": ["weave"],
                   "assert_max_kbs": 150, "assert_min_tick_hz": 30, "assert_max_tick_ms": 0},
            "reference": {"clients": 128, "patterns": ["idle", "weave"],
                          "assert_max_kbs": 150, "assert_min_tick_hz": 58, "assert_max_tick_ms": 16.6},
        },
    }
    p = tmp_path / "scale-gate.json"
    p.write_text(json.dumps(cfg), encoding="utf-8")
    return p


def test_load_config_missing_raises(tmp_path):
    with pytest.raises(ValueError, match="not found"):
        sg.load_config(tmp_path / "nope.json")


def test_load_config_garbled_raises(tmp_path):
    p = tmp_path / "bad.json"
    p.write_text("{ not json", encoding="utf-8")
    with pytest.raises(ValueError, match="not valid JSON"):
        sg.load_config(p)


def test_load_profile_known_and_defaults(tmp_path):
    cfg = sg.load_config(_write_config(tmp_path))
    prof = sg.load_profile(cfg, "pr")
    assert prof["clients"] == 64
    assert prof["patterns"] == ["weave"]
    # default filled in (not present in the 'pr' profile)
    assert prof["sim_worker_threads"] == 0
    # leading-underscore meta keys are stripped
    assert "_comment" not in prof


def test_load_profile_unknown_raises(tmp_path):
    cfg = sg.load_config(_write_config(tmp_path))
    with pytest.raises(ValueError, match="unknown profile"):
        sg.load_profile(cfg, "bogus")


# ---- assert_flags --------------------------------------------------------------------------------
def test_assert_flags_omits_zero_and_renders_ints():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(assert_max_kbs=150, assert_min_tick_hz=30, assert_max_tick_ms=0)
    flags = sg.assert_flags(prof, strict=False)
    assert flags == ["--assert-max-kbs", "150", "--assert-min-tick-hz", "30"]
    assert "--assert-max-tick-ms" not in flags  # 0 -> disabled


def test_assert_flags_tick_ms_only_when_strict():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(assert_max_kbs=0, assert_min_tick_hz=0, assert_max_tick_ms=16.6)
    assert sg.assert_flags(prof, strict=False) == []  # advisory -> omitted
    assert sg.assert_flags(prof, strict=True) == ["--assert-max-tick-ms", "16.6"]


# ---- evaluate_report -----------------------------------------------------------------------------
def _report(kbs_max=66.0, tick_hz_min=60.0, tick_p99=10.0, connected=64, requested=64,
            disconnected=0, with_server=True, rss_kb=200000, rss_startup_kb=200000,
            load_factor=1.0, dropped_ticks=0, congestion_min_hz=60.0, congestion_recovered_hz=60.0,
            sensing=None, rss_slope="omit", rss_step_kb=None, rss_step_at_s=None):
    r = {
        "clients_requested": requested,
        "clients_connected": connected,
        "clients_disconnected": disconnected,
        "downstream_kbs_per_client": {"max": kbs_max, "mean": kbs_max},
        "observed_server_tick_hz": {"min": tick_hz_min},
    }
    # rss_slope: "omit" leaves the key absent (no series); None emits null (series too short); a
    # number emits the fitted tail slope (#789).
    if rss_slope != "omit":
        r["rss_slope_kb_per_min"] = rss_slope
    # The isolated step the slope ignores by design, surfaced alongside it (#1095).
    if rss_step_kb is not None:
        r["rss_step_max_kb"] = rss_step_kb
        r["rss_step_at_s"] = rss_step_at_s
    if with_server:
        r["server_tick"] = {"tick_ms": {"p99": tick_p99}, "rss_kb": rss_kb, "rss_startup_kb": rss_startup_kb,
                            "load_factor": load_factor, "dropped_ticks": dropped_ticks,
                            "congestion_min_send_hz": congestion_min_hz,
                            "congestion_recovered_send_hz": congestion_recovered_hz}
        if sensing is not None:
            r["server_tick"]["sensing_ms"] = sensing
            r["server_tick"]["tick_ms"]["mean"] = 5.0
    return r


def _profile(**over):
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(assert_max_kbs=150, assert_min_tick_hz=30, assert_max_tick_ms=16.6)
    prof.update(over)
    return prof


def test_evaluate_pass():
    ev = sg.evaluate_report(_report(), _profile(), strict=True)
    assert ev["passed"]


def test_evaluate_fail_on_kbs():
    ev = sg.evaluate_report(_report(kbs_max=200.0), _profile(), strict=True)
    assert not ev["passed"]


def test_evaluate_fail_on_admission():
    ev = sg.evaluate_report(_report(connected=60), _profile(), strict=True)
    assert not ev["passed"]


def test_evaluate_fail_on_tick_hz():
    ev = sg.evaluate_report(_report(tick_hz_min=20.0), _profile(), strict=True)
    assert not ev["passed"]


def test_evaluate_tick_ms_strict_vs_advisory():
    # p99 over budget: fails under strict, passes (advisory) otherwise.
    over = _report(tick_p99=25.0)
    assert not sg.evaluate_report(over, _profile(), strict=True)["passed"]
    assert sg.evaluate_report(over, _profile(), strict=False)["passed"]


def test_evaluate_missing_server_block_when_tick_ms_enabled():
    # Mirrors test_bot_swarm.cpp:290 — assert enabled but no server metrics -> fail (strict).
    r = _report(with_server=False)
    ev = sg.evaluate_report(r, _profile(), strict=True)
    assert not ev["passed"]
    check = next(c for c in ev["checks"] if c["name"] == "server_tick.tick_ms.p99")
    assert not check["ok"]


# ---- soak RSS leak gate (#707) -------------------------------------------------------------------
def test_assert_flags_emits_rss_growth_when_set():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(assert_max_rss_growth_kb=262144)
    assert sg.assert_flags(prof, strict=False) == ["--assert-max-rss-growth-kb", "262144"]


def test_evaluate_pass_on_rss_growth_within_cap():
    prof = _profile(assert_max_rss_growth_kb=262144)
    r = _report(rss_startup_kb=200000, rss_kb=240000)  # +40 MiB
    assert sg.evaluate_report(r, prof, strict=True)["passed"]


def test_evaluate_fail_on_rss_growth_over_cap():
    prof = _profile(assert_max_rss_growth_kb=262144)
    r = _report(rss_startup_kb=200000, rss_kb=600000)  # +~390 MiB
    ev = sg.evaluate_report(r, prof, strict=True)
    assert not ev["passed"]
    check = next(c for c in ev["checks"] if c["name"] == "server_tick.rss_growth_kb")
    assert not check["ok"]


def test_evaluate_missing_server_block_when_rss_enabled():
    prof = _profile(assert_max_rss_growth_kb=262144)
    ev = sg.evaluate_report(_report(with_server=False), prof, strict=True)
    assert not ev["passed"]
    check = next(c for c in ev["checks"] if c["name"] == "server_tick.rss_growth_kb")
    assert not check["ok"]


# ---- soak RSS leak TREND gate (#789) -------------------------------------------------------------
def test_assert_flags_emits_rss_slope_when_set():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(assert_max_rss_slope_kb_per_min=128)
    assert sg.assert_flags(prof, strict=False) == ["--assert-max-rss-slope-kb-per-min", "128"]


def test_evaluate_pass_on_flat_rss_slope():
    prof = _profile(assert_max_rss_slope_kb_per_min=128)
    assert sg.evaluate_report(_report(rss_slope=3.0), prof, strict=True)["passed"]  # plateau


def test_evaluate_fail_on_rss_slope_over_cap():
    prof = _profile(assert_max_rss_slope_kb_per_min=128)
    ev = sg.evaluate_report(_report(rss_slope=250.0), prof, strict=True)  # slow leak
    assert not ev["passed"]
    check = next(c for c in ev["checks"] if c["name"] == "rss_slope_kb_per_min")
    assert not check["ok"]


def test_rss_slope_check_names_the_step_it_ignored():
    # The slope ignores one isolated step by design (#1095), so a passing gate must still SAY a
    # multi-MB allocation happened — otherwise the step disappears behind a green check.
    prof = _profile(assert_max_rss_slope_kb_per_min=128)
    ev = sg.evaluate_report(_report(rss_slope=8.8, rss_step_kb=6100, rss_step_at_s=5250.0), prof, strict=True)
    assert ev["passed"]
    check = next(c for c in ev["checks"] if c["name"] == "rss_slope_kb_per_min")
    assert "6.0 MB" in check["detail"] and "t=5250s" in check["detail"]

    # No step in the tail -> nothing appended, and a zero step is not "a step".
    plain = sg.evaluate_report(_report(rss_slope=8.8), prof, strict=True)
    assert "largest tail step" not in next(
        c for c in plain["checks"] if c["name"] == "rss_slope_kb_per_min")["detail"]
    zero = sg.evaluate_report(_report(rss_slope=8.8, rss_step_kb=0, rss_step_at_s=0.0), prof, strict=True)
    assert "largest tail step" not in next(
        c for c in zero["checks"] if c["name"] == "rss_slope_kb_per_min")["detail"]


def test_evaluate_fail_on_missing_rss_slope_when_enabled():
    prof = _profile(assert_max_rss_slope_kb_per_min=128)
    # null slope (series too short) and an absent key both fail — cannot pass an unchecked gate.
    assert not sg.evaluate_report(_report(rss_slope=None), prof, strict=True)["passed"]
    assert not sg.evaluate_report(_report(rss_slope="omit"), prof, strict=True)["passed"]


# ---- compare_baseline ----------------------------------------------------------------------------
def test_compare_baseline_none_is_noop():
    res = sg.compare_baseline(_report(), None, 10)
    assert not res["regressed"]


def test_compare_baseline_boundary():
    # baseline 100, +10% tolerance -> limit 110.
    assert not sg.compare_baseline({"downstream_kbs_per_client": {"mean": 110.0}}, 100.0, 10)["regressed"]
    assert sg.compare_baseline({"downstream_kbs_per_client": {"mean": 110.5}}, 100.0, 10)["regressed"]


def test_compare_baseline_improvement_never_regresses():
    assert not sg.compare_baseline({"downstream_kbs_per_client": {"mean": 50.0}}, 100.0, 10)["regressed"]


# ---- overrun governor gate (#574) ----------------------------------------------------------------
def test_assert_flags_emits_governor_asserts_when_enabled():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(assert_max_load_factor=0.99, assert_max_dropped_ticks=0)
    flags = sg.assert_flags(prof, strict=False)
    assert "--assert-max-load-factor" in flags
    assert flags[flags.index("--assert-max-load-factor") + 1] == "0.99"
    assert "--assert-max-dropped-ticks" in flags
    assert flags[flags.index("--assert-max-dropped-ticks") + 1] == "0"


def test_assert_flags_omits_governor_asserts_when_negative():
    # Negative sentinel = disabled (0 is a real value for both, so it must NOT be the off-switch).
    prof = dict(sg.PROFILE_DEFAULTS)  # defaults are -1.0 / -1
    assert "--assert-max-load-factor" not in sg.assert_flags(prof, strict=False)
    assert "--assert-max-dropped-ticks" not in sg.assert_flags(prof, strict=False)


def test_evaluate_governor_engaged_pass_and_never_engaged_fail():
    prof = _profile(assert_max_load_factor=0.99, assert_max_dropped_ticks=0)
    # Governor shed under load -> load_factor < 1, no drops -> pass.
    assert sg.evaluate_report(_report(load_factor=0.70, dropped_ticks=0), prof, strict=True)["passed"]
    # Governor never engaged -> load_factor stayed 1.0 -> fail.
    ev = sg.evaluate_report(_report(load_factor=1.0), prof, strict=True)
    assert not ev["passed"]
    check = next(c for c in ev["checks"] if c["name"] == "server_tick.load_factor")
    assert not check["ok"]


def test_evaluate_governor_dropped_ticks_and_missing_server():
    prof = _profile(assert_max_load_factor=0.99, assert_max_dropped_ticks=0)
    # Sim spiralled and dropped ticks -> fail.
    ev = sg.evaluate_report(_report(load_factor=0.70, dropped_ticks=5), prof, strict=True)
    assert not ev["passed"]
    assert not next(c for c in ev["checks"] if c["name"] == "server_tick.dropped_ticks")["ok"]
    # Assert enabled but no server block -> fail (cannot evaluate).
    ev2 = sg.evaluate_report(_report(with_server=False), prof, strict=True)
    assert not ev2["passed"]


def test_expand_runs_sets_governor_env():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(patterns=["weave"], entity_spawn_counts=[5000], sim_worker_threads_sweep=[1], governor=True)
    runs = sg.expand_runs(prof)
    assert len(runs) == 1
    assert runs[0]["env"]["FL_LOADTEST_GOVERNOR"] == "1"
    assert runs[0]["env"]["FL_TEST_SPAWN_AI"] == "5000"
    assert runs[0]["env"]["FL_SIM_WORKER_THREADS"] == "1"
    assert runs[0]["flags"] == ["--assert-min-entities", "5000"]


def test_committed_overrun_profile_loads_with_governor_on():
    # The shipped scale-gate.json overrun profile must carry the governor + gate values (#574).
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    prof = sg.load_profile(cfg, "overrun")
    assert prof["governor"] is True
    assert prof["baselined"] is False
    assert prof["assert_max_load_factor"] == 0.99
    # A small dropped-ticks allowance absorbs the startup-spawn transient; a spiral produces far more.
    assert prof["assert_max_dropped_ticks"] == 100
    assert prof["entity_spawn_counts"] == [5000]
    assert prof["sim_worker_threads_sweep"] == [1]


# ---- congestion gate (#714) ----------------------------------------------------------------------
def test_assert_flags_emits_congestion_asserts_when_enabled():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(assert_congestion_engaged_hz=30, assert_congestion_recovered_hz=55)
    flags = sg.assert_flags(prof, strict=False)
    assert flags[flags.index("--assert-congestion-engaged-hz") + 1] == "30"
    assert flags[flags.index("--assert-congestion-recovered-hz") + 1] == "55"
    # 0 = disabled: the defaults emit neither flag.
    off = sg.assert_flags(dict(sg.PROFILE_DEFAULTS), strict=False)
    assert "--assert-congestion-engaged-hz" not in off
    assert "--assert-congestion-recovered-hz" not in off


def test_degrade_flags_forward_the_lossy_proxy_window():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(degrade_start_s=25, degrade_duration_s=30, degrade_loss=0.15, degrade_delay_ms=100)
    flags = sg.degrade_flags(prof)
    assert flags[flags.index("--degrade-duration") + 1] == "30"
    assert flags[flags.index("--degrade-start") + 1] == "25"
    assert flags[flags.index("--degrade-loss") + 1] == "0.15"
    assert flags[flags.index("--degrade-delay-ms") + 1] == "100"
    # No degrade window (the default) -> no proxy flags at all.
    assert sg.degrade_flags(dict(sg.PROFILE_DEFAULTS)) == []


def test_evaluate_congestion_engaged_and_recovered():
    prof = _profile(assert_congestion_engaged_hz=30, assert_congestion_recovered_hz=55)
    # Engaged (min fell to 10) and recovered (back to 60) -> pass.
    assert sg.evaluate_report(_report(congestion_min_hz=10.0, congestion_recovered_hz=60.0),
                              prof, strict=True)["passed"]
    # Never engaged (min stuck at 60) -> fail on the engaged check.
    ev = sg.evaluate_report(_report(congestion_min_hz=60.0), prof, strict=True)
    assert not ev["passed"]
    assert not next(c for c in ev["checks"] if c["name"] == "server_tick.congestion_min_send_hz")["ok"]
    # Engaged but never recovered -> fail on the recovered check.
    ev2 = sg.evaluate_report(_report(congestion_min_hz=10.0, congestion_recovered_hz=20.0),
                             prof, strict=True)
    assert not ev2["passed"]
    assert not next(c for c in ev2["checks"]
                    if c["name"] == "server_tick.congestion_recovered_send_hz")["ok"]
    # Missing server block while enabled -> fail.
    assert not sg.evaluate_report(_report(with_server=False), prof, strict=True)["passed"]


def test_committed_congestion_profile_loads_with_degrade_window():
    # The shipped scale-gate.json congestion profile must carry the proxy window + gates (#714).
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    prof = sg.load_profile(cfg, "congestion")
    assert prof["baselined"] is False
    assert prof["degrade_duration_s"] == 30
    # 5% loss stays below ENet's own peer-timeout threshold (15% dropped a client in a dev run);
    # the 100 ms delay is the deterministic engage trigger.
    assert prof["degrade_loss"] == 0.05
    assert prof["degrade_delay_ms"] == 100
    assert prof["assert_congestion_engaged_hz"] == 30
    assert prof["assert_congestion_recovered_hz"] == 55
    # The window must open after the ramp and close well before the run ends (recovery tail).
    assert prof["degrade_start_s"] + prof["degrade_duration_s"] < prof["duration_s"]


# ---- expand_runs (entity-scale sweep, #573) ------------------------------------------------------
def test_expand_runs_normal_profile_is_one_run_per_pattern():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(patterns=["idle", "weave"])
    runs = sg.expand_runs(prof)
    assert [r["label"] for r in runs] == ["idle", "weave"]
    # No entity-scale env or flags on a normal profile.
    assert all(r["env"] == {} and r["flags"] == [] for r in runs)


def test_expand_runs_entity_scale_cartesian_product():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(patterns=["weave"], entity_spawn_counts=[0, 2000], sim_worker_threads_sweep=[1, 8])
    runs = sg.expand_runs(prof)
    # 1 pattern x 2 counts x 2 workers = 4 runs, unique labels.
    assert [r["label"] for r in runs] == ["weave_e0_w1", "weave_e0_w8", "weave_e2000_w1", "weave_e2000_w8"]
    # env carries the sweep knobs.
    assert runs[3]["env"] == {"FL_TEST_SPAWN_AI": "2000", "FL_SIM_WORKER_THREADS": "8"}
    # --assert-min-entities only when count > 0.
    assert runs[0]["flags"] == []  # e0
    assert runs[3]["flags"] == ["--assert-min-entities", "2000"]


# ---- AI mix + projectile churn (#580) ------------------------------------------------------------
def test_expand_runs_sets_mix_and_churn_env():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(patterns=["weave"], entity_spawn_counts=[2000], sim_worker_threads_sweep=[8],
                ai_mix="loiter:60,pursuit:25,patrol:15", projectile_rate=120, projectile_ttl_s=3.0)
    runs = sg.expand_runs(prof)
    assert len(runs) == 1
    env = runs[0]["env"]
    assert env["FL_TEST_SPAWN_MIX"] == "loiter:60,pursuit:25,patrol:15"
    assert env["FL_TEST_PROJECTILE_RATE"] == "120"
    assert env["FL_TEST_PROJECTILE_TTL_S"] == "3.0"
    assert env["FL_TEST_SPAWN_AI"] == "2000"


def test_expand_runs_omits_mix_and_churn_env_by_default():
    prof = dict(sg.PROFILE_DEFAULTS)
    prof.update(patterns=["weave"])
    runs = sg.expand_runs(prof)
    assert "FL_TEST_SPAWN_MIX" not in runs[0]["env"]
    assert "FL_TEST_PROJECTILE_RATE" not in runs[0]["env"]


def test_committed_entity_churn_profile_loads():
    # The shipped scale-gate.json entity-churn profile must carry the mix + churn knobs (#580).
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    prof = sg.load_profile(cfg, "entity-churn")
    assert prof["baselined"] is False
    assert prof["ai_mix"] == "loiter:60,pursuit:25,patrol:15"
    assert prof["projectile_rate"] == 120
    assert prof["entity_spawn_counts"] == [500, 2000, 5000]
    assert prof["sim_worker_threads_sweep"] == [1, 8]


# ---- runner_for_platform -------------------------------------------------------------------------
def test_runner_for_platform():
    assert sg.runner_for_platform("win32") == "run_loadtest.ps1"
    assert sg.runner_for_platform("linux") == "run_loadtest.sh"
    assert sg.runner_for_platform("darwin") == "run_loadtest.sh"


# ---- render_summary ------------------------------------------------------------------------------
def test_render_summary_pass_and_fail():
    results = [
        {"pattern": "weave", "passed": True,
         "checks": [{"name": "admission", "ok": True, "detail": "64/64", "advisory": False}],
         "baseline": {"regressed": False, "detail": "66 vs 66"}},
        {"pattern": "idle", "passed": False,
         "checks": [{"name": "downstream_kbs_per_client.max", "ok": False, "detail": "200 <= 150",
                     "advisory": False}],
         "baseline": {"regressed": True, "detail": "200 vs 66"}},
    ]
    md = sg.render_summary("reference", results)
    assert "profile `reference`" in md
    assert "✅ pass" in md and "❌ FAIL" in md
    assert "REGRESSED" in md


# ---- write_summary -------------------------------------------------------------------------------
def test_write_summary_to_github_step_summary(tmp_path, monkeypatch):
    out = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(out))
    sg.write_summary("hello")
    assert "hello" in out.read_text(encoding="utf-8")


def test_write_summary_to_stdout(capsys, monkeypatch):
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    sg.write_summary("to-stdout")
    assert "to-stdout" in capsys.readouterr().out


def test_write_summary_unwritable_path_does_not_raise(capsys, monkeypatch, tmp_path):
    # The #1242 defect: an unwritable GITHUB_STEP_SUMMARY (deleted dir, read-only file, full disk)
    # raised out of write_summary and flipped the gate verdict over a reporting problem. The
    # summary must still echo, the failure is a stderr note, and the caller's verdict is untouched.
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(tmp_path / "no-such-dir" / "summary.md"))
    sg.write_summary("still-reported")
    captured = capsys.readouterr()
    assert "still-reported" in captured.out
    assert "could not write GITHUB_STEP_SUMMARY" in captured.err


# ---- main(): exit-code aggregation (runner monkeypatched, no sockets) ----------------------------
def _setup_main(tmp_path, monkeypatch, report, runner_code=0):
    cfg = _write_config(tmp_path)
    baseline = tmp_path / "baseline.json"
    baseline.write_text(json.dumps({"kbs": {"pr/weave": 66.0}}), encoding="utf-8")
    report_path = tmp_path / "report.json"
    report_path.write_text(json.dumps(report), encoding="utf-8")

    def fake_run_pattern(*_args, **_kwargs):
        return runner_code, report_path

    monkeypatch.setattr(sg, "run_pattern", fake_run_pattern)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    return ["--profile", "pr", "--build-dir", "x", "--config", str(cfg), "--baseline", str(baseline)]


def test_main_all_pass(tmp_path, monkeypatch):
    argv = _setup_main(tmp_path, monkeypatch, _report(kbs_max=66.0))
    assert sg.main(argv) == 0


def test_main_runner_nonzero_fails(tmp_path, monkeypatch):
    argv = _setup_main(tmp_path, monkeypatch, _report(kbs_max=66.0), runner_code=1)
    assert sg.main(argv) == 1


def test_main_baseline_regression_fails(tmp_path, monkeypatch):
    # mean 100 vs baseline 66 (+10% = 72.6) -> regression even though absolute kbs<150 passes.
    argv = _setup_main(tmp_path, monkeypatch, _report(kbs_max=100.0))
    assert sg.main(argv) == 1


def test_main_missing_report_fails(tmp_path, monkeypatch):
    cfg = _write_config(tmp_path)

    def fake_run_pattern(*a, **k):
        return 1, None

    monkeypatch.setattr(sg, "run_pattern", fake_run_pattern)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    assert sg.main(["--profile", "pr", "--build-dir", "x", "--config", str(cfg),
                    "--baseline", str(tmp_path / "none.json")]) == 1


def test_main_update_baseline_refuses_partial(tmp_path, monkeypatch):
    # A failed run must not silently write an incomplete baseline.
    cfg = _write_config(tmp_path)
    baseline = tmp_path / "bl.json"

    def fake_run_pattern(*a, **k):
        return 1, None

    monkeypatch.setattr(sg, "run_pattern", fake_run_pattern)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    rc = sg.main(["--profile", "pr", "--build-dir", "x", "--config", str(cfg),
                  "--baseline", str(baseline), "--update-baseline"])
    assert rc == 1
    assert not baseline.exists()


def test_main_update_baseline_roundtrip(tmp_path, monkeypatch):
    argv = _setup_main(tmp_path, monkeypatch, _report(kbs_max=80.0))
    baseline_path = argv[argv.index("--baseline") + 1]
    rc = sg.main(argv + ["--update-baseline"])
    assert rc == 0
    data = json.loads(Path(baseline_path).read_text(encoding="utf-8"))
    assert data["kbs"]["pr/weave"] == pytest.approx(80.0)


# ---- entity-scale profile (#573): advisory, never baselined ------------------------------------
def _write_entity_scale_config(tmp_path):
    cfg = {
        "kbs_baseline_tolerance_pct": 10,
        "profiles": {
            "entity-scale": {
                "clients": 64, "duration_s": 60, "patterns": ["weave"],
                "entity_spawn_counts": [0, 2000], "sim_worker_threads_sweep": [1, 4],
                "baselined": False,
                "assert_max_kbs": 0, "assert_min_tick_hz": 0, "assert_max_tick_ms": 16.6,
            },
        },
    }
    p = tmp_path / "scale-gate.json"
    p.write_text(json.dumps(cfg), encoding="utf-8")
    return p


def test_main_entity_scale_passes_and_never_writes_baseline(tmp_path, monkeypatch):
    cfg = _write_entity_scale_config(tmp_path)
    baseline = tmp_path / "bl.json"
    baseline.write_text(json.dumps({"kbs": {"pr/weave": 66.0}}), encoding="utf-8")
    report = _report(kbs_max=66.0)
    report["server_tick"]["entities"] = 2048  # advisory detail source
    report_path = tmp_path / "report.json"
    report_path.write_text(json.dumps(report), encoding="utf-8")
    monkeypatch.setattr(sg, "run_pattern", lambda *a, **k: (0, report_path))
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    # advisory-only (kbs asserts disabled) -> passes; baseline file must be untouched.
    before = baseline.read_text(encoding="utf-8")
    rc = sg.main(["--profile", "entity-scale", "--build-dir", "x", "--config", str(cfg),
                  "--baseline", str(baseline), "--strict"])
    assert rc == 0
    assert baseline.read_text(encoding="utf-8") == before


def test_main_update_baseline_refuses_non_baselined_profile(tmp_path, monkeypatch):
    cfg = _write_entity_scale_config(tmp_path)
    baseline = tmp_path / "bl.json"
    monkeypatch.setattr(sg, "run_pattern", lambda *a, **k: (0, None))
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    rc = sg.main(["--profile", "entity-scale", "--build-dir", "x", "--config", str(cfg),
                  "--baseline", str(baseline), "--update-baseline"])
    assert rc == 1
    assert not baseline.exists()


# ---- transport (#649) ----------------------------------------------------------------------------
def test_default_profile_transport_is_enet():
    # bot_swarm stays the enet6 regression instrument: every pre-existing profile must be unchanged.
    assert sg.PROFILE_DEFAULTS["transport"] == "enet"


def test_evaluate_passes_when_the_measured_transport_matches_the_profile():
    r = _report()
    r["transport"] = "gns"
    ev = sg.evaluate_report(r, _profile(transport="gns"), strict=True)
    assert ev["passed"]


def test_evaluate_fails_a_gns_profile_that_actually_measured_enet():
    # The whole point of the gate: a GNS run that silently fell back to enet6 must FAIL, not pass.
    # bot_swarm refuses the fallback outright; this is the gate-layer backstop (and it catches a
    # stale binary that predates the transport plumbing).
    r = _report()
    r["transport"] = "enet"
    ev = sg.evaluate_report(r, _profile(transport="gns"), strict=True)
    assert not ev["passed"]
    assert any(c["name"] == "transport" and not c["ok"] for c in ev["checks"])


def test_pre_v3_reports_without_a_transport_key_are_treated_as_enet():
    ev = sg.evaluate_report(_report(), _profile(), strict=True)  # no "transport" key at all
    assert ev["passed"]


def test_reference_profile_is_gns_and_baselined_per_transport():
    # #773: the PRIMARY reference profile pins both ends to GNS — the default internet transport is
    # the one the headline numbers describe.
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    prof = sg.load_profile(cfg, "reference")
    assert prof["transport"] == "gns"
    assert prof["clients"] == 128
    # Baselined, but keyed per PROFILE — so each transport gets its own keys. That is what makes it
    # safe: GNS's wire bytes are legitimately not enet6's (enet6 range-coder-compresses; GNS encrypts
    # and does not compress), so the two must never be diffed against one another (#772).
    assert prof["baselined"] is True


def test_reference_enet_profile_keeps_the_enet6_regression_leg_at_scale():
    # enet6 stays a gate leg (#773): the LAN/single-player backend keeps a 128-client strict-tier
    # run, mirroring `reference` so the two transports remain directly comparable.
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    prof = sg.load_profile(cfg, "reference-enet")
    assert prof["transport"] == "enet"
    assert prof["clients"] == 128
    assert prof["patterns"] == ["idle", "weave", "aggressive"]
    assert prof["baselined"] is True


def test_characterisation_profiles_run_on_the_shipping_transport():
    # #773: soak + every characterisation profile measures GNS — published figures describe what
    # ships. (enet6 regression coverage = `pr` on every PR + `reference-enet` on the strict tier.)
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    for name in ("soak", "entity-scale", "overrun", "congestion", "entity-churn"):
        assert sg.load_profile(cfg, name)["transport"] == "gns", name


def test_sweep_profiles_carry_no_tick_ms_assert():
    # The reference-runner workflow leg always passes --strict, which turns assert_max_tick_ms into
    # a hard bot_swarm assert. The sweeps' deliberately-collapsing single-worker cells ARE the
    # characterisation, so the two sweep profiles must not carry a tick-ms threshold (#773).
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    for name in ("entity-scale", "entity-churn"):
        assert sg.load_profile(cfg, name)["assert_max_tick_ms"] == 0, name


def test_gns_profiles_set_the_runner_transport_env():
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    runs = sg.expand_runs(sg.load_profile(cfg, "reference"))
    assert all(r["env"].get("FL_LOADTEST_TRANSPORT") == "gns" for r in runs)


def test_enet_profiles_do_not_set_the_transport_env():
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    for name in ("pr", "reference-enet"):
        runs = sg.expand_runs(sg.load_profile(cfg, name))
        assert all("FL_LOADTEST_TRANSPORT" not in r["env"] for r in runs), name


# ---- wire-byte baseline (#772) -------------------------------------------------------------------
def _wire_report(wire_kbs=31.9, **kw):
    r = _report(**kw)
    r["server_tick"]["wire_out_kbs_per_client"] = wire_kbs
    return r


def test_wire_kbs_reads_the_server_tick_block():
    assert sg.wire_kbs(_wire_report(wire_kbs=24.4)) == 24.4
    assert sg.wire_kbs(_report()) == 0.0  # pre-v6 server report: no wire keys


def test_wire_baseline_regression_fails_the_run():
    cmp = sg.compare_wire_baseline(_wire_report(wire_kbs=40.0), 24.4, 10)
    assert cmp["regressed"]


def test_wire_baseline_within_tolerance_passes():
    cmp = sg.compare_wire_baseline(_wire_report(wire_kbs=25.0), 24.4, 10)
    assert not cmp["regressed"]


def test_wire_baseline_improvement_never_fails():
    cmp = sg.compare_wire_baseline(_wire_report(wire_kbs=10.0), 24.4, 10)
    assert not cmp["regressed"]


def test_missing_wire_baseline_is_not_a_regression():
    cmp = sg.compare_wire_baseline(_wire_report(), None, 10)
    assert not cmp["regressed"]


def test_pre_v6_server_report_is_skipped_not_treated_as_a_regression_to_zero():
    # A server that predates the wire keys reports nothing; that must not read as "0 KB/s, improved"
    # (which would silently rewrite the baseline to zero on the next --update-baseline).
    cmp = sg.compare_wire_baseline(_report(), 24.4, 10)
    assert not cmp["regressed"]
    assert "pre-v6" in cmp["detail"]


# ---- wire ceiling replaces the payload ceiling (#772) --------------------------------------------
def test_wire_ceiling_fails_when_wire_bytes_exceed_it():
    ev = sg.evaluate_report(_wire_report(wire_kbs=200.0), _profile(assert_max_wire_kbs=150), strict=True)
    assert not ev["passed"]
    assert any(c["name"] == "wire_out_kbs_per_client" and not c["ok"] for c in ev["checks"])


def test_wire_ceiling_passes_under_the_limit():
    ev = sg.evaluate_report(_wire_report(wire_kbs=80.5), _profile(assert_max_wire_kbs=150), strict=True)
    assert ev["passed"]


def test_a_server_that_cannot_report_wire_bytes_fails_the_wire_ceiling():
    # Silently passing a ceiling you cannot measure is how a gate becomes decorative.
    ev = sg.evaluate_report(_report(), _profile(assert_max_wire_kbs=150), strict=True)
    assert not ev["passed"]


def test_shipped_profiles_gate_on_wire_not_payload():
    # The 150 KB/s ceiling moved to WIRE bytes: payload KB/s is transport-independent and so cannot
    # see what a transport actually costs to run (enet6 range-coder-compresses, GNS does not).
    cfg = sg.load_config(sg.DEFAULT_CONFIG)
    for name in ("pr", "reference", "reference-enet", "soak"):
        prof = sg.load_profile(cfg, name)
        assert prof["assert_max_wire_kbs"] == 150, name
        assert prof["assert_max_kbs"] == 0, name


# --- sensing phase (#685/#686) ------------------------------------------------


def test_sensing_phase_is_reported_as_advisory():
    """A sensing regression must be VISIBLE, but must not fail a run on a number we invented."""
    report = _report(sensing={"mean": 0.5, "p99": 1.2})
    result = sg.evaluate_report(report, _profile(), strict=True)

    sensing = [c for c in result["checks"] if c["name"] == "server_tick.sensing_ms"]
    assert len(sensing) == 1
    assert sensing[0]["advisory"] is True
    assert sensing[0]["ok"] is True
    assert "0.500" in sensing[0]["detail"]
    assert "10.0% of tick" in sensing[0]["detail"]  # 0.5 of a 5.0 ms tick


def test_sensing_phase_absent_is_not_a_check():
    """An older metrics file simply has no sensing block. That is a missing field, not a failure —
    the report format is additive and name-keyed, which is exactly why it needs no schema bump."""
    report = _report()  # no sensing_ms
    result = sg.evaluate_report(report, _profile(), strict=True)
    assert not [c for c in result["checks"] if c["name"] == "server_tick.sensing_ms"]


def test_sensing_advisory_never_fails_the_gate():
    """Even a pathological sensing cost passes: tick_ms.p99 is the backstop that actually fails."""
    report = _report(sensing={"mean": 99.0, "p99": 99.0})
    result = sg.evaluate_report(report, _profile(), strict=True)
    sensing = [c for c in result["checks"] if c["name"] == "server_tick.sensing_ms"][0]
    assert sensing["advisory"] is True
    assert sensing["ok"] is True


# --- #1089: the gate must measure a POPULATED world ----------------------------------------------

def test_headline_profiles_are_sensor_loaded():
    """`pr`, `reference` and `soak` must fly a sensor-carrying aircraft.

    This is the instrument, not a detail. With the sensorless default every ContactTable is empty,
    so datalink team fusion -- the largest O(P^2) cost in the server -- merges nothing and costs
    nothing, and the committed 128-client numbers describe a hollow battlespace. A gate that cannot
    fail on the bug it exists to catch is not evidence.
    """
    config = sg.load_config(sg.DEFAULT_CONFIG)
    for name in ("pr", "reference", "soak"):
        profile = sg.load_profile(config, name)
        assert profile["entity_type"] == "builtin:sensor-fighter", name
        # ...and something to detect: sensors with an empty sky still fuse nothing. Deliberately
        # ai_entity_count and NOT entity_spawn_counts -- the latter is the entity-scale sweep and
        # carries an exact-count --assert-min-entities that a headline profile should not gate on.
        assert profile["ai_entity_count"] > 0, name
        assert not profile.get("entity_spawn_counts"), name


def test_entity_scale_keeps_its_hollow_sweep():
    """`entity-scale` measures entity-pool and spatial-index cost against entity count.

    Loading sensors onto it would confound the one variable it exists to isolate, so it deliberately
    does NOT get the treatment the headline profiles get.
    """
    config = sg.load_config(sg.DEFAULT_CONFIG)
    profile = sg.load_profile(config, "entity-scale")
    assert profile["entity_type"] == ""


def test_entity_type_reaches_bot_swarm_as_a_flag():
    """The profile key has to actually arrive at bot_swarm, or the profile is decorative."""
    profile = dict(sg.PROFILE_DEFAULTS)
    profile["entity_type"] = "builtin:sensor-fighter"
    flags = sg.assert_flags(profile, strict=False)
    assert "--entity-type" in flags
    assert flags[flags.index("--entity-type") + 1] == "builtin:sensor-fighter"


def test_no_entity_type_emits_no_flag():
    """An unset entity_type must leave the command line exactly as it was before #1089."""
    profile = dict(sg.PROFILE_DEFAULTS)
    assert "--entity-type" not in sg.assert_flags(profile, strict=False)


def test_ai_entity_count_populates_the_world_without_an_entity_count_assert():
    """Populating the sky must not drag the entity-scale sweep's exact-count gate along with it.

    entity_spawn_counts emits --assert-min-entities, which is right for a profile whose subject IS
    the entity count and wrong for a headline profile: a couple of loiter entities dying mid-run
    would fail the gate while saying nothing about tick time or bandwidth.
    """
    profile = dict(sg.PROFILE_DEFAULTS)
    profile["ai_entity_count"] = 64
    runs = sg.expand_runs(profile)
    assert runs
    for r in runs:
        env = r["env"] if isinstance(r, dict) else r.env
        flags = r["flags"] if isinstance(r, dict) else r.flags
        assert env.get("FL_TEST_SPAWN_AI") == "64"
        assert "--assert-min-entities" not in flags
