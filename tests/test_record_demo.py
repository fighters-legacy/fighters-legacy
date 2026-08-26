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


def test_parse_content_probe_sums_ffmpeg_output():
    text = (
        "[blackdetect @ 0x1] black_start:0 black_end:10.5 black_duration:10.5\n"
        "[blackdetect @ 0x1] black_start:12 black_end:14 black_duration:2\n"
        "[Parsed_freezedetect_1 @ 0x2] lavfi.freezedetect.freeze_duration: 8.25\n"
    )
    black, frozen = parse_content_probe(text)
    assert black == pytest.approx(12.5)
    assert frozen == pytest.approx(8.25)


def test_parse_content_probe_ignores_unrelated_output():
    black, frozen = parse_content_probe("frame= 100 fps=30 q=20.0 size=1kB\nDuration: 00:00:22.60\n")
    assert (black, frozen) == (0.0, 0.0)


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
