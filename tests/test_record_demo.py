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
