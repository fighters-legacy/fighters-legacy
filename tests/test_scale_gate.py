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
            disconnected=0, with_server=True):
    r = {
        "clients_requested": requested,
        "clients_connected": connected,
        "clients_disconnected": disconnected,
        "downstream_kbs_per_client": {"max": kbs_max, "mean": kbs_max},
        "observed_server_tick_hz": {"min": tick_hz_min},
    }
    if with_server:
        r["server_tick"] = {"tick_ms": {"p99": tick_p99}}
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
