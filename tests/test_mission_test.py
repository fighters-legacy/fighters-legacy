# SPDX-FileCopyrightText: Contributors to Fighters Legacy
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/mission_test/mission_test.py — pure assertion logic, no fl-server needed."""

from __future__ import annotations


from conftest import load_tool

_mod = load_tool("mission_test", "tools", "mission_test", "mission_test.py")
evaluate_report = _mod.evaluate_report

_OK = {"outcome": "success", "triggers_fired": 2, "live_entities": 2, "spawned_objects": 2}


def test_passing_report_has_no_failures():
    assert evaluate_report(_OK, expect_outcome="success", min_triggers=2, min_survivors=2, min_spawned=2) == []


def test_wrong_outcome_fails():
    failures = evaluate_report(_OK, expect_outcome="failure")
    assert len(failures) == 1 and "outcome" in failures[0]


def test_too_few_triggers_fails():
    failures = evaluate_report(_OK, min_triggers=5)
    assert len(failures) == 1 and "triggers_fired" in failures[0]


def test_too_few_survivors_fails():
    failures = evaluate_report({"outcome": "success", "live_entities": 0}, min_survivors=1)
    assert len(failures) == 1 and "live_entities" in failures[0]


def test_multiple_failures_accumulate():
    failures = evaluate_report({"outcome": "failure", "triggers_fired": 0, "spawned_objects": 0},
                               expect_outcome="success", min_triggers=1, min_spawned=1)
    assert len(failures) == 3


def test_no_expectations_always_passes():
    assert evaluate_report({"outcome": "incomplete"}) == []


def test_entity_cap_refusals_fail_even_with_no_expectations():
    """A truncated world is never a valid mission result, whatever was asserted (#1049)."""
    failures = evaluate_report({"outcome": "success", "entity_cap_refusals": 3})
    assert len(failures) == 1 and "entity_cap_refusals" in failures[0]


def test_absent_entity_cap_refusals_is_treated_as_zero():
    """Reports written before the field existed must not start failing."""
    assert evaluate_report(_OK, expect_outcome="success", min_spawned=2) == []
