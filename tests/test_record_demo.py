# SPDX-FileCopyrightText: Contributors to Fighters Legacy
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/record_demo/record_demo.py — pure helpers, no fl-server / renderer needed."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from conftest import load_tool

_mod = load_tool("record_demo", "tools", "record_demo", "record_demo.py")
load_manifest = _mod.load_manifest
duration_ok = _mod.duration_ok
wall_clock_cap = _mod.wall_clock_cap
parse_content_probe = _mod.parse_content_probe
content_ok = _mod.content_ok
distinct_frames_ok = _mod.distinct_frames_ok
shot_list_end = _mod.shot_list_end
manifest_drift = _mod.manifest_drift
parse_ydif_series = _mod.parse_ydif_series
frozen_runs = _mod.frozen_runs
shot_windows = _mod.shot_windows
frozen_shot_problems = _mod.frozen_shot_problems


def test_load_manifest_reads_entries(tmp_path):
    p = tmp_path / "demos.json"
    p.write_text(json.dumps({"demos": [
        {"id": "a", "mission_file": "a.yaml", "out_name": "a", "expected_duration": 30},
    ]}))
    demos = load_manifest(str(p))
    assert len(demos) == 1
    assert demos[0]["id"] == "a"


def test_load_manifest_rejects_incomplete_entry(tmp_path):
    p = tmp_path / "demos.json"
    p.write_text(json.dumps({"demos": [{"id": "a", "mission_file": "a.yaml"}]}))
    with pytest.raises(ValueError):
        load_manifest(str(p))


def test_duration_ok_accepts_close_match():
    ok, _ = duration_ok(actual=44.0, expected=44)
    assert ok
    # A shorter video (mission ended early) is still fine down to the floor.
    ok, _ = duration_ok(actual=20.0, expected=44)
    assert ok


def test_duration_ok_rejects_empty():
    ok, reason = duration_ok(actual=0.0, expected=44)
    assert not ok
    assert "empty" in reason


def test_duration_ok_rejects_overrun():
    ok, reason = duration_ok(actual=100.0, expected=44)
    assert not ok
    assert "too long" in reason


def test_duration_ok_rejects_too_short():
    ok, reason = duration_ok(actual=0.5, expected=44)
    assert not ok
    assert "too short" in reason


def test_repo_demos_manifest_is_valid():
    """The committed missions/demos/demos.json parses and every mission file it names exists."""
    demos_dir = Path(__file__).parent.parent / "missions" / "demos"
    demos = load_manifest(str(demos_dir / "demos.json"))
    assert len(demos) >= 5
    for d in demos:
        assert (demos_dir / d["mission_file"]).is_file(), d["mission_file"]
        assert d["expected_duration"] > 0


# --- #1347: the wall-clock cap is not the shot list ----------------------------------------------


def test_wall_clock_cap_scales_with_the_time_rate():
    """The defect: a flat 180 s cap at --time-rate eighth buys 22.5 s of shot list, so every demo
    recorded to exactly 22.6 s whatever its shots said."""
    assert wall_clock_cap(38.0, "eighth") > 38.0 * 8
    assert wall_clock_cap(38.0, "quarter") > 38.0 * 4
    assert wall_clock_cap(38.0, "normal") > 38.0
    # Slower rate => strictly more wall clock for the same shot list.
    assert wall_clock_cap(50.0, "eighth") > wall_clock_cap(50.0, "quarter") > wall_clock_cap(50.0, "normal")


def test_wall_clock_cap_would_have_covered_the_truncated_demos():
    """demo-dogfight's 50 s and demo-gallery-flyover's 38 s of shots, at the rate that truncated them."""
    for shots in (50.0, 38.0):
        assert wall_clock_cap(shots, "eighth") >= shots * 8


def test_wall_clock_cap_honours_an_explicit_request():
    assert wall_clock_cap(38.0, "eighth", requested=42) == 42


def test_wall_clock_cap_rejects_a_paused_sim():
    with pytest.raises(ValueError):
        wall_clock_cap(38.0, "paused")


def test_wall_clock_cap_treats_an_unknown_rate_as_normal():
    assert wall_clock_cap(10.0, "wat") == wall_clock_cap(10.0, "normal")


# --- #1347: the gate must check the PAYLOAD, not just the envelope -------------------------------


def test_parse_content_probe_sums_black_and_measures_frozen_from_one_pass():
    text = (
        "[blackdetect @ 0x1] black_start:0 black_end:10.5 black_duration:10.5\n"
        "[blackdetect @ 0x1] black_start:12 black_end:14 black_duration:2\n"
    ) + _ydif_log([0.0] * 90)  # 3 s of one repeated frame
    black, frozen, runs = parse_content_probe(text)
    assert black == pytest.approx(12.5)
    assert frozen == pytest.approx(3.0, abs=0.05)
    assert len(runs) == 1


def test_parse_content_probe_ignores_unrelated_output():
    black, frozen, runs = parse_content_probe(
        "frame= 100 fps=30 q=20.0 size=1kB\nDuration: 00:00:22.60\n")
    assert (black, frozen, runs) == (0.0, 0.0, [])


def test_content_ok_fails_an_all_black_video():
    """The exact #1347 recording: 22.6 s of one black frame, blessed by the duration check."""
    ok, reason = content_ok(22.6, black_s=22.6, frozen_s=22.6)
    assert not ok
    assert "BLACK" in reason


def test_content_ok_fails_a_frozen_video():
    ok, reason = content_ok(38.0, black_s=0.0, frozen_s=30.0)
    assert not ok
    assert "FROZEN" in reason


def test_content_ok_accepts_a_real_recording():
    """A demo may legitimately open on a dark or held shot; only a run that is MOSTLY dead fails."""
    ok, reason = content_ok(38.0, black_s=2.0, frozen_s=3.0)
    assert ok
    assert "picture ok" in reason


def test_content_ok_fails_an_empty_video():
    assert not content_ok(0.0, 0.0, 0.0)[0]


def test_distinct_frames_ok_fails_one_repeated_frame():
    ok, reason = distinct_frames_ok(["deadbeef"] * 32)
    assert not ok
    assert "never changed" in reason


def test_distinct_frames_ok_accepts_a_moving_picture():
    assert distinct_frames_ok([f"h{i}" for i in range(32)])[0]


def test_distinct_frames_ok_fails_on_no_frames():
    assert not distinct_frames_ok([])[0]


def test_distinct_frames_ok_does_not_demand_more_frames_than_exist():
    """A very short sequence is judged on what it has, not on the nominal minimum."""
    assert distinct_frames_ok(["a", "b", "c"], min_distinct=8)[0]


# --- #1378: demos.json and the shot lists it describes must agree ---------------------------------
# The two numbers are load-bearing in DIFFERENT places -- the recorder derives --record-max-sec from
# the shot list (#1347) while the acceptance window comes from demos.json -- so drift between them
# does not fail loudly, it caps a run and then judges it against a length it was never given the
# wall clock to reach. demo-atc-scramble's shots ran to 128 s while the manifest declared the 44 s
# it had carried unchanged since #909, and only a hands-on recording pass found it.


def test_shot_list_end_is_the_last_shot_end():
    mission = {"cameras": {"shots": [
        {"type": "static", "start": 0, "duration": 22},
        {"type": "move", "start": 22, "duration": 12},
        {"type": "static", "start": 34, "duration": 18},
    ]}}
    assert shot_list_end(mission) == 52


def test_shot_list_end_uses_the_latest_end_not_the_last_entry():
    """Shots are authored in start order by convention, not by rule -- take the max, not the tail."""
    mission = {"cameras": {"shots": [
        {"type": "static", "start": 30, "duration": 40},
        {"type": "static", "start": 0, "duration": 10},
    ]}}
    assert shot_list_end(mission) == 70


def test_shot_list_end_of_a_mission_with_no_shots_is_zero():
    assert shot_list_end({}) == 0.0
    assert shot_list_end({"cameras": {}}) == 0.0
    assert shot_list_end(None) == 0.0


def _require_yaml():
    """Fail, never skip, when PyYAML is missing.

    A drift guard that quietly skips itself is decorative -- the same failure this file exists to
    prevent, one layer up. CI installs PyYAML for this step; a dev without it gets a message naming
    what to install rather than a green run that checked nothing.
    """
    try:
        import yaml  # noqa: F401  (presence is the assertion)
    except ImportError:  # pragma: no cover - only on a machine without PyYAML
        pytest.fail("PyYAML is required by the demos.json drift check (pip install pyyaml)")


def _write_mission(dir_path, name, shots):
    body = "name: t\ncameras:\n  shots:\n"
    for shot in shots:
        body += f"    - {{ type: static, start: {shot[0]}, duration: {shot[1]} }}\n"
    (dir_path / name).write_text(body, encoding="utf-8")


def test_manifest_drift_reports_a_mismatch_with_both_numbers(tmp_path):
    """The exact #1378 shape: a re-authored shot list, a manifest nobody updated with it."""
    _require_yaml()
    _write_mission(tmp_path, "a.yaml", [(0, 22), (22, 12), (34, 18), (52, 40), (92, 36)])
    drift = manifest_drift(
        [{"id": "a", "mission_file": "a.yaml", "out_name": "a", "expected_duration": 44}], tmp_path)
    assert len(drift) == 1
    assert "128" in drift[0] and "44" in drift[0]


def test_manifest_drift_is_empty_when_they_agree(tmp_path):
    _require_yaml()
    _write_mission(tmp_path, "a.yaml", [(0, 20), (20, 24)])
    assert manifest_drift(
        [{"id": "a", "mission_file": "a.yaml", "out_name": "a", "expected_duration": 44}],
        tmp_path) == []


def test_manifest_drift_flags_a_mission_with_no_shots(tmp_path):
    """A mission the recorder cannot frame is drift too, not agreement by absence."""
    _require_yaml()
    (tmp_path / "a.yaml").write_text("name: t\n", encoding="utf-8")
    drift = manifest_drift(
        [{"id": "a", "mission_file": "a.yaml", "out_name": "a", "expected_duration": 44}], tmp_path)
    assert len(drift) == 1
    assert "no camera shots" in drift[0]


def test_manifest_drift_flags_a_missing_mission_file(tmp_path):
    _require_yaml()
    drift = manifest_drift(
        [{"id": "a", "mission_file": "gone.yaml", "out_name": "a", "expected_duration": 44}],
        tmp_path)
    assert len(drift) == 1
    assert "not found" in drift[0]


def test_committed_demo_set_agrees_with_its_shot_lists():
    """The guard itself, on the shipped set. This is the check the pair never had."""
    _require_yaml()
    demos_dir = Path(__file__).parent.parent / "missions" / "demos"
    demos = load_manifest(str(demos_dir / "demos.json"))
    assert manifest_drift(demos, demos_dir) == []


# --- #1378: a frozen stretch belongs to a shot, and a moving shot must not have one ----------------
# content_ok is an ENVELOPE: more than half the run frozen. It passed demo-night-patrol at 21.6s of
# 39.7s frozen only because that crossed the line; at 13s it would have said "picture ok" about a
# recording whose orbit shot rendered one still image for its whole length. Where the picture stopped
# moving is the question -- a static shot on a quiet scene is SUPPOSED to be still, an orbiting
# camera is not, and the recorder's timeline is the shot list's, so the timestamp names the shot.

# Two lines per frame, exactly as `signalstats,metadata=print` emits them.
def _ydif_log(values, fps=30.0):
    out = []
    for i, v in enumerate(values):
        out.append(f"[Parsed_metadata_1 @ 0x1] frame:{i}  pts:{i}  pts_time:{i / fps:.6f}")
        out.append(f"[Parsed_metadata_1 @ 0x1] lavfi.signalstats.YDIF={v}")
    return "\n".join(out) + "\n"


NIGHT_PATROL_SHOTS = [(0.0, 10.0, "static"), (10.0, 22.0, "orbit"),
                      (22.0, 32.0, "move"), (32.0, 40.0, "static")]


def test_parse_ydif_series_pairs_each_timestamp_with_its_difference():
    series = parse_ydif_series(_ydif_log([0.0, 0.5, 0.25]))
    assert [round(t, 4) for t, _ in series] == [0.0, 0.0333, 0.0667]
    assert [v for _, v in series] == [0.0, 0.5, 0.25]


def test_parse_ydif_series_drops_a_value_with_no_timestamp():
    """Never guess which frame an orphan value belongs to — a wrong timestamp is a wrong shot."""
    assert parse_ydif_series("[x] lavfi.signalstats.YDIF=0.5\n") == []


def test_parse_ydif_series_of_a_probe_that_printed_nothing_is_empty():
    assert parse_ydif_series("") == []
    assert parse_ydif_series("ffmpeg version 7.1\n") == []


def test_frozen_runs_finds_a_still_stretch_and_not_a_moving_one():
    """A recording of one repeated frame — the #1347 failure this check exists for — measures 0."""
    still = [0.0] * 90          # 3 s at 30 fps
    moving = [0.4] * 90
    assert frozen_runs(parse_ydif_series(_ydif_log(still + moving))) == \
        [(0.0, pytest.approx(3.0, abs=0.05))]
    assert frozen_runs(parse_ydif_series(_ydif_log(moving))) == []


def test_frozen_runs_ignores_a_stretch_shorter_than_the_minimum():
    """A slow pan over smooth sky repeats a few frames; that is not a dead recording."""
    assert frozen_runs(parse_ydif_series(_ydif_log([0.4] * 30 + [0.0] * 30 + [0.4] * 30))) == []


def test_frozen_runs_reports_a_stretch_that_runs_to_the_end_of_the_clip():
    """The recording that stops updating and never recovers must not fall off the end unreported."""
    runs = frozen_runs(parse_ydif_series(_ydif_log([0.4] * 30 + [0.0] * 120)))
    assert len(runs) == 1
    assert runs[0][1] == pytest.approx(4.0, abs=0.1)


def test_frozen_runs_uses_the_floor_not_exact_equality():
    """Encoding noise means a held frame is *nearly*, not exactly, identical."""
    assert frozen_runs(parse_ydif_series(_ydif_log([0.0004] * 90))) != []
    assert frozen_runs(parse_ydif_series(_ydif_log([0.0006] * 90))) == []


def test_shot_windows_are_start_end_type():
    mission = {"cameras": {"shots": [
        {"type": "static", "start": 0, "duration": 10},
        {"type": "orbit", "start": 10, "duration": 12},
    ]}}
    assert shot_windows(mission) == [(0.0, 10.0, "static"), (10.0, 22.0, "orbit")]


def test_a_static_shot_may_hold_a_still_picture():
    """The 0-10s freeze in demo-night-patrol is a static camera on a quiet scene: correct."""
    assert frozen_shot_problems([(0.0, 9.7)], NIGHT_PATROL_SHOTS) == []


def test_an_orbit_shot_rendering_a_still_picture_is_a_defect():
    """The #1378 finding: 21.6s of 39.7s frozen, and only the third shot ever changed the image.

    An orbiting camera moves by definition, so a frozen orbit means the shot has nothing in it or
    its target is gone -- which is exactly what #1381 turned out to be.
    """
    still = frozen_runs(parse_ydif_series(_ydif_log([0.4] * 300 + [0.0] * 348)))  # moving to t=10, then still
    problems = frozen_shot_problems(still, NIGHT_PATROL_SHOTS)
    assert len(problems) == 1
    assert "orbit shot at 10-22s" in problems[0]


def test_a_freeze_spanning_a_boundary_names_only_the_moving_shots():
    problems = frozen_shot_problems([(8.0, 6.0)], NIGHT_PATROL_SHOTS)  # 8-14s: static then orbit
    assert len(problems) == 1
    assert "orbit" in problems[0] and "static" not in problems[0]


def test_a_brief_still_run_on_a_moving_shot_is_not_flagged():
    """A slow pan over smooth sky legitimately repeats a couple of frames; the check is for a shot
    that rendered nothing, not for a compression artefact."""
    assert frozen_shot_problems([(12.0, 1.0)], NIGHT_PATROL_SHOTS) == []


def test_a_freeze_past_the_end_of_the_shot_list_is_not_attributed():
    """Nothing to blame a shot for after the shots have run out."""
    assert frozen_shot_problems([(45.0, 5.0)], NIGHT_PATROL_SHOTS) == []


def test_every_kind_of_moving_shot_counts():
    shots = [(0.0, 10.0, "orbit"), (10.0, 20.0, "chase"), (20.0, 30.0, "move")]
    problems = frozen_shot_problems([(1.0, 5.0), (11.0, 5.0), (21.0, 5.0)], shots)
    assert len(problems) == 3


def test_freeze_floor_is_calibrated_between_dead_footage_and_real_motion():
    """The floor is the whole check (#1378), so pin it against the medians it was measured from.

    It replaced ffmpeg's `freezedetect`, whose response to its own threshold is NOT monotone on real
    footage — sweeping one recording gave 0.0s at n=0.1, 25.7s at n=0.01, 0.0s at n=0.001 and 6.6s at
    n=0.0002. A tighter threshold reporting more frozen time, in both directions, cannot be
    calibrated at all. A run of frames below a YDIF floor is monotone by construction.
    """
    dead_footage = 0.000094    # cameras 2 km out over empty ground
    deliberate_hold = 0.00014  # a static shot on a quiet scene
    real_motion = 0.0035       # the same demo re-framed 60 m off the track
    busiest_demo = 0.21        # demo-dogfight, a 2v2 merge filling the frame
    assert dead_footage < _mod.FREEZE_YDIF < real_motion < busiest_demo
    assert deliberate_hold < _mod.FREEZE_YDIF
    # A repeated frame — the failure the check exists for — measures 0 and keeps the whole margin.
    assert 0.0 < _mod.FREEZE_YDIF
