# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/ai_eval/ai_eval.py (#599).

Pure-logic coverage only — no network, no model, no validate-mission binary. Per the initiative's
CI policy (docs/ai-architecture.md §7) CI must never require a model, so everything exercised here
is the extraction/scoring/aggregation layer; the HTTP and subprocess edges are not touched.
"""

import importlib.util
import json
from pathlib import Path

import pytest

_MODULE_PATH = Path(__file__).resolve().parent.parent / "tools" / "ai_eval" / "ai_eval.py"
_spec = importlib.util.spec_from_file_location("ai_eval", _MODULE_PATH)
ae = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ae)


# ---- strip_code_fence / extract_json_object ----------------------------------------------------


def test_strip_code_fence_unfenced_passthrough():
    assert ae.strip_code_fence('{"command": "rejoin"}') == '{"command": "rejoin"}'


def test_strip_code_fence_extracts_fenced_body():
    text = 'Here you go:\n```json\n{"command": "rejoin"}\n```\nHope that helps!'
    assert ae.strip_code_fence(text) == '{"command": "rejoin"}'


def test_strip_code_fence_handles_none():
    assert ae.strip_code_fence(None) == ""


def test_extract_json_object_plain():
    assert ae.extract_json_object('{"command": "cover_me"}') == {"command": "cover_me"}


def test_extract_json_object_fenced():
    assert ae.extract_json_object('```json\n{"a": 1}\n```') == {"a": 1}


def test_extract_json_object_with_surrounding_prose():
    # The common small-model failure: correct object buried in chatter.
    text = 'Sure! The answer is {"command": "hold_fire"} — let me know if you need more.'
    assert ae.extract_json_object(text) == {"command": "hold_fire"}


def test_extract_json_object_nested_braces_and_strings():
    text = 'noise {"a": {"b": "}"}, "c": 2} trailing'
    assert ae.extract_json_object(text) == {"a": {"b": "}"}, "c": 2}


def test_extract_json_object_rejects_non_object():
    assert ae.extract_json_object("[1, 2, 3]") is None


def test_extract_json_object_returns_none_on_garbage():
    assert ae.extract_json_object("I cannot help with that.") is None
    assert ae.extract_json_object("") is None


# ---- extract_yaml_document ---------------------------------------------------------------------


def test_extract_yaml_document_strips_fence_and_prose():
    text = "Here is the mission:\n```yaml\nname: Test\nmap: ukraine\n```"
    assert ae.extract_yaml_document(text) == "name: Test\nmap: ukraine"


def test_extract_yaml_document_skips_leading_prose_to_first_key():
    text = "Sure, this should work.\n\nname: Test\nmap: ukraine\n"
    assert ae.extract_yaml_document(text) == "name: Test\nmap: ukraine"


def test_extract_yaml_document_empty_when_no_mapping():
    assert ae.extract_yaml_document("I'm sorry, I can't do that.") == ""


# ---- score_intent ------------------------------------------------------------------------------

_GRAMMAR = ["attack_my_target", "rejoin", "unknown"]


def _intent_case(expected):
    return {"id": "c", "expect": {"command": expected}, "_grammar": _GRAMMAR}


def test_score_intent_correct():
    r = ae.score_intent(_intent_case("rejoin"), '{"command": "rejoin"}')
    assert (r["parsed"], r["schema_valid"], r["correct"]) == (True, True, True)


def test_score_intent_wrong_command_is_schema_valid_but_incorrect():
    r = ae.score_intent(_intent_case("rejoin"), '{"command": "attack_my_target"}')
    assert (r["parsed"], r["schema_valid"], r["correct"]) == (True, True, False)


def test_score_intent_hallucinated_command_fails_schema():
    # The load-bearing case: a command outside the grammar must never count as schema-valid.
    r = ae.score_intent(_intent_case("rejoin"), '{"command": "eject"}')
    assert (r["parsed"], r["schema_valid"], r["correct"]) == (True, False, False)


def test_score_intent_unparseable():
    r = ae.score_intent(_intent_case("rejoin"), "Roger, rejoining now!")
    assert (r["parsed"], r["schema_valid"], r["correct"]) == (False, False, False)


def test_score_intent_declining_out_of_grammar_call_is_correct():
    r = ae.score_intent(_intent_case("unknown"), '{"command": "unknown"}')
    assert r["correct"] is True


# ---- score_ops ---------------------------------------------------------------------------------

_CAUSES = ["tick_overrun", "congestion", "healthy"]
_ALLOW = ["status", "tickstats", "kick"]


def _ops_case(expected):
    return {
        "id": "c",
        "expect": {"root_cause": expected},
        "_causes": _CAUSES,
        "_allowlist": _ALLOW,
    }


def test_score_ops_correct_with_allowlisted_actions():
    r = ae.score_ops(
        _ops_case("tick_overrun"),
        '{"root_cause": "tick_overrun", "actions": ["tickstats", "status"], "severity": "high"}',
    )
    assert (r["correct"], r["actions_allowed"], r["schema_valid"]) == (True, True, True)


def test_score_ops_action_outside_allowlist_fails_even_with_right_cause():
    # An agent inventing a command it was never permitted to run is a failure, full stop.
    r = ae.score_ops(
        _ops_case("tick_overrun"),
        '{"root_cause": "tick_overrun", "actions": ["rm -rf /", "status"]}',
    )
    assert r["schema_valid"] is True
    assert r["actions_allowed"] is False
    assert r["correct"] is False


def test_score_ops_empty_actions_are_allowed():
    r = ae.score_ops(_ops_case("healthy"), '{"root_cause": "healthy", "actions": []}')
    assert (r["correct"], r["actions_allowed"]) == (True, True)


def test_score_ops_missing_actions_key_treated_as_empty():
    r = ae.score_ops(_ops_case("healthy"), '{"root_cause": "healthy"}')
    assert (r["correct"], r["actions_allowed"]) == (True, True)


def test_score_ops_unknown_cause_fails_schema():
    r = ae.score_ops(_ops_case("healthy"), '{"root_cause": "gremlins", "actions": []}')
    assert (r["schema_valid"], r["correct"]) == (False, False)


def test_score_ops_unparseable():
    r = ae.score_ops(_ops_case("healthy"), "The server looks fine to me.")
    assert (r["parsed"], r["correct"]) == (False, False)


# ---- check_mission_semantics -------------------------------------------------------------------

_MISSION_CASE = {
    "expect": {"sides": ["nato", "russia"], "min_objects": 2, "weather_preset": "storm"}
}

_GOOD_YAML = """name: Test
sides:
  - nato
  - russia
weather:
  preset: storm
objects:
  - type: F22
    id: a
  - type: SA10
    id: b
"""


_GOOD_YAML_FLOW = """name: Test
sides: [nato, russia]
weather: { preset: storm }
objects:
  - type: F22
    id: a
  - type: SA10
    id: b
"""


def test_check_mission_semantics_accepts_conforming_doc():
    assert ae.check_mission_semantics(_MISSION_CASE, _GOOD_YAML) == []


def test_check_mission_semantics_accepts_flow_style_sequences():
    # Regression: models copy whichever style the prompt example used. A block-only check scored
    # valid `sides: [nato, russia]` documents as failures and made capable models look incapable.
    assert ae.check_mission_semantics(_MISSION_CASE, _GOOD_YAML_FLOW) == []


def test_parse_yaml_sequence_block_style():
    assert ae.parse_yaml_sequence(_GOOD_YAML, "sides") == ["nato", "russia"]


def test_parse_yaml_sequence_flow_style():
    assert ae.parse_yaml_sequence(_GOOD_YAML_FLOW, "sides") == ["nato", "russia"]


def test_parse_yaml_sequence_quoted_items():
    assert ae.parse_yaml_sequence('sides: ["nato", \'russia\']\n', "sides") == ["nato", "russia"]


def test_parse_yaml_sequence_stops_at_next_top_level_key():
    text = "sides:\n  - nato\nobjects:\n  - type: F22\n"
    assert ae.parse_yaml_sequence(text, "sides") == ["nato"]


def test_parse_yaml_sequence_missing_key():
    assert ae.parse_yaml_sequence("name: Test\n", "sides") == []


def test_check_mission_semantics_flags_missing_side():
    yaml = _GOOD_YAML.replace("  - russia\n", "")
    errors = ae.check_mission_semantics(_MISSION_CASE, yaml)
    assert any("russia" in e for e in errors)


def test_check_mission_semantics_flags_too_few_objects():
    yaml = _GOOD_YAML.replace("  - type: SA10\n    id: b\n", "")
    errors = ae.check_mission_semantics(_MISSION_CASE, yaml)
    assert any("objects" in e for e in errors)


def test_check_mission_semantics_flags_wrong_weather_preset():
    yaml = _GOOD_YAML.replace("preset: storm", "preset: clear")
    errors = ae.check_mission_semantics(_MISSION_CASE, yaml)
    assert any("storm" in e for e in errors)


# ---- summarize_latencies / aggregate -----------------------------------------------------------


def test_summarize_latencies_empty():
    assert ae.summarize_latencies([]) == {
        "min": 0.0, "mean": 0.0, "p50": 0.0, "p95": 0.0, "max": 0.0
    }


def test_summarize_latencies_basic():
    s = ae.summarize_latencies([1.0, 2.0, 3.0, 4.0])
    assert s["min"] == 1.0
    assert s["max"] == 4.0
    assert s["p50"] == 2.5
    assert s["p95"] == 4.0


def test_aggregate_rates_and_budget():
    cases = [
        {"parsed": True, "schema_valid": True, "correct": True, "latency_s": 1.0},
        {"parsed": True, "schema_valid": True, "correct": False, "latency_s": 1.5},
        {"parsed": False, "schema_valid": False, "correct": False, "latency_s": 2.0},
    ]
    m = ae.aggregate("intent", 2.0, cases)
    assert m["cases"] == 3
    assert m["parse_rate"] == pytest.approx(0.667, abs=0.001)
    assert m["accuracy"] == pytest.approx(0.333, abs=0.001)
    assert m["within_budget"] is True


def test_aggregate_flags_budget_violation():
    cases = [{"parsed": True, "schema_valid": True, "correct": True, "latency_s": 9.0}]
    m = ae.aggregate("intent", 2.0, cases)
    assert m["within_budget"] is False


def test_aggregate_mission_reports_pass_at_1_and_after_repair():
    cases = [
        {"parsed": True, "schema_valid": True, "correct": True, "valid_first_try": True, "latency_s": 5.0},
        {"parsed": True, "schema_valid": True, "correct": True, "valid_first_try": False, "latency_s": 9.0},
        {"parsed": True, "schema_valid": False, "correct": False, "valid_first_try": False, "latency_s": 7.0},
    ]
    m = ae.aggregate("mission", 60.0, cases)
    assert m["pass_at_1"] == pytest.approx(0.333, abs=0.001)
    assert m["pass_after_repair"] == pytest.approx(0.667, abs=0.001)


def test_aggregate_counts_transport_errors():
    cases = [{"error": "HTTP 500", "parsed": False, "schema_valid": False, "correct": False, "latency_s": None}]
    m = ae.aggregate("ops", 60.0, cases)
    assert m["errors"] == 1
    assert m["within_budget"] is False


# ---- suites ship valid + self-consistent -------------------------------------------------------


@pytest.mark.parametrize("name", ae.SUITE_NAMES)
def test_shipped_suite_loads(name):
    suite = ae.load_suite(ae.suite_path(name))
    assert suite["name"] == name
    assert suite["budget_s"] > 0
    ids = [c["id"] for c in suite["cases"]]
    assert len(ids) == len(set(ids)), "case ids must be unique"


def test_intent_suite_expectations_are_inside_the_grammar():
    suite = ae.load_suite(ae.suite_path("intent"))
    for case in suite["cases"]:
        assert case["expect"]["command"] in suite["grammar"]


def test_ops_suite_expectations_are_inside_the_cause_enum():
    suite = ae.load_suite(ae.suite_path("ops"))
    for case in suite["cases"]:
        assert case["expect"]["root_cause"] in suite["root_causes"]


def test_load_suite_rejects_missing_file(tmp_path):
    with pytest.raises(ValueError, match="not found"):
        ae.load_suite(tmp_path / "nope.json")


def test_load_suite_rejects_missing_key(tmp_path):
    p = tmp_path / "bad.json"
    p.write_text(json.dumps({"name": "x"}), encoding="utf-8")
    with pytest.raises(ValueError, match="missing required key"):
        ae.load_suite(p)


def test_load_suite_rejects_empty_cases(tmp_path):
    p = tmp_path / "empty.json"
    p.write_text(
        json.dumps({"name": "x", "system_prompt": "s", "budget_s": 1, "cases": []}),
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="no cases"):
        ae.load_suite(p)


# ---- render_markdown ---------------------------------------------------------------------------


def test_render_markdown_emits_a_row_per_model_suite():
    metrics = ae.aggregate(
        "intent", 2.0, [{"parsed": True, "schema_valid": True, "correct": True, "latency_s": 1.0}]
    )
    out = ae.render_markdown([("m1", metrics), ("m2", metrics)])
    assert out.count("\n") == 3  # header + separator + 2 rows
    assert "`m1`" in out and "`m2`" in out
