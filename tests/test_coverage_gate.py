# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/coverage_gate.py.

The property under test is the one #1128 was about: the gate must never conflate "under-covered"
with "not measured". Both halves need pinning — a test that only proves the threshold path would
have passed happily for the three weeks the real gate was aborting during parse and reporting it
as a coverage failure.

gcovr is stubbed with a script so the measurement-failure paths (parse abort, no summary, garbage
summary, empty report) are exercised for real, through subprocess, without needing gcov data.
"""

import json
import os
import stat
from pathlib import Path

import pytest

from conftest import load_tool

cg = load_tool("coverage_gate", "tools", "coverage_gate.py")


def _pct(covered, total):
    return 100.0 * covered / total if total else 0.0  # what gcovr itself reports for an empty scope


def summary(line=(700, 1000), branch=(800, 1000), function=(90, 100), files=None):
    """A gcovr --json-summary payload with the fields the gate reads."""
    return {
        "root": ".",
        "gcovr/summary_format_version": "0.6",
        "files": files if files is not None else [
            {"filename": "engine/net/WorldBroadcaster.cpp", "line_total": 600, "line_covered": 420,
             "branch_total": 500, "branch_covered": 450, "function_total": 60, "function_covered": 55},
            {"filename": "engine/render/TerrainStreamer.cpp", "line_total": 400, "line_covered": 280,
             "branch_total": 500, "branch_covered": 350, "function_total": 40, "function_covered": 35},
        ],
        "line_covered": line[0], "line_total": line[1], "line_percent": _pct(*line),
        "branch_covered": branch[0], "branch_total": branch[1], "branch_percent": _pct(*branch),
        "function_covered": function[0], "function_total": function[1], "function_percent": _pct(*function),
    }


def write_summary(tmp_path, payload):
    path = tmp_path / "coverage-summary.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def stub_gcovr(tmp_path, body):
    """A fake gcovr on disk. `body` is shell run with "$@" available."""
    path = tmp_path / "gcovr-stub"
    path.write_text("#!/usr/bin/env bash\n" + body + "\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return path


# ---- threshold verdicts (a number WAS produced) --------------------------------------------------


def test_pass_when_every_metric_clears(tmp_path, capsys):
    path = write_summary(tmp_path, summary(line=(760, 1000), branch=(840, 1000)))
    rc = cg.main(["--evaluate", str(path), "--min-line", "70", "--min-branch", "80"])
    assert rc == cg.EXIT_PASS
    assert "coverage gate passed" in capsys.readouterr().out


def test_branch_below_threshold_names_the_measured_number(tmp_path, capsys):
    path = write_summary(tmp_path, summary(branch=(784, 1000)))
    rc = cg.main(["--evaluate", str(path), "--min-line", "70", "--min-branch", "80"])
    captured = capsys.readouterr()
    assert rc == cg.EXIT_THRESHOLD
    # The measured percentage and the limit both appear, and the message says which one is which.
    assert "COVERAGE BELOW THRESHOLD" in captured.err
    assert "78.40%" in captured.err and "80.0%" in captured.err
    assert "Coverage below threshold" in captured.out  # the ::error annotation


def test_line_below_threshold_is_reported_independently(tmp_path, capsys):
    path = write_summary(tmp_path, summary(line=(690, 1000), branch=(900, 1000)))
    rc = cg.main(["--evaluate", str(path), "--min-line", "70", "--min-branch", "80"])
    assert rc == cg.EXIT_THRESHOLD
    assert "line 69.00% < 70.0%" in capsys.readouterr().err


def test_exactly_on_the_threshold_passes(tmp_path):
    path = write_summary(tmp_path, summary(line=(700, 1000), branch=(800, 1000)))
    assert cg.main(["--evaluate", str(path), "--min-line", "70", "--min-branch", "80"]) == cg.EXIT_PASS


def test_no_threshold_means_report_only(tmp_path):
    path = write_summary(tmp_path, summary(line=(10, 1000), branch=(10, 1000)))
    assert cg.main(["--evaluate", str(path)]) == cg.EXIT_PASS


# ---- measurement failures (NO number exists) -----------------------------------------------------


def test_gcovr_parse_abort_is_a_measurement_failure_not_a_threshold_miss(tmp_path, capsys):
    """The #1128 failure exactly: gcovr dies on the gcov negative-hits bug and exits 64."""
    gcovr = stub_gcovr(tmp_path, """
echo "NegativeHits: engine/net/SnapshotCompression.cpp:40 Got negative hit value in: branch  2 taken -26" >&2
echo "(ERROR) Error occurred while reading reports: Worker thread raised exception, workers canceled." >&2
exit 64
""")
    rc = cg.main(["--gcovr", str(gcovr), "--filter", "engine/", "--min-branch", "80", "--min-line", "70",
                  "--summary-json", str(tmp_path / "s.json")])
    captured = capsys.readouterr()
    assert rc == cg.EXIT_UNMEASURED
    assert "NO COVERAGE NUMBER WAS PRODUCED" in captured.err
    assert "exited 64" in captured.err
    # It must not claim a coverage verdict, and gcovr's own diagnosis must survive.
    assert "BELOW THRESHOLD" not in captured.err
    assert "NegativeHits" in captured.err


def test_gcovr_success_but_no_summary_file(tmp_path, capsys):
    gcovr = stub_gcovr(tmp_path, "exit 0")
    rc = cg.main(["--gcovr", str(gcovr), "--min-branch", "80", "--summary-json", str(tmp_path / "missing.json")])
    assert rc == cg.EXIT_UNMEASURED
    assert "wrote no summary file" in capsys.readouterr().err


def test_unparseable_summary_is_a_measurement_failure(tmp_path, capsys):
    path = tmp_path / "s.json"
    path.write_text("{ this is not json", encoding="utf-8")
    rc = cg.main(["--evaluate", str(path), "--min-branch", "80"])
    assert rc == cg.EXIT_UNMEASURED
    assert "not valid JSON" in capsys.readouterr().err


def test_empty_report_is_not_zero_percent(tmp_path, capsys):
    """gcovr exits 0 and reports 0.0% when the filters match nothing. 0.0% is not a measurement."""
    path = write_summary(tmp_path, summary(line=(0, 0), branch=(0, 0), function=(0, 0), files=[]))
    rc = cg.main(["--evaluate", str(path), "--min-branch", "80", "--min-line", "70"])
    captured = capsys.readouterr()
    assert rc == cg.EXIT_UNMEASURED
    assert "covers no files" in captured.err
    assert "0.00%" not in captured.err


def test_zero_denominator_is_not_a_percentage(tmp_path, capsys):
    path = write_summary(tmp_path, summary(branch=(0, 0)))
    rc = cg.main(["--evaluate", str(path), "--min-branch", "80", "--min-line", "70"])
    assert rc == cg.EXIT_UNMEASURED
    assert "zero branch entities" in capsys.readouterr().err


def test_impossible_counts_are_rejected(tmp_path, capsys):
    path = write_summary(tmp_path, summary(branch=(1200, 1000)))
    rc = cg.main(["--evaluate", str(path), "--min-branch", "80"])
    assert rc == cg.EXIT_UNMEASURED
    assert "impossible" in capsys.readouterr().err


def test_missing_gcovr_executable_is_a_measurement_failure(tmp_path, capsys):
    rc = cg.main(["--gcovr", str(tmp_path / "definitely-not-here"), "--min-branch", "80",
                  "--summary-json", str(tmp_path / "s.json")])
    assert rc == cg.EXIT_UNMEASURED
    assert "gcovr not found" in capsys.readouterr().err


def test_function_metric_absent_does_not_fail_an_ungated_run(tmp_path):
    payload = summary()
    del payload["function_total"]
    del payload["function_covered"]
    path = write_summary(tmp_path, payload)
    assert cg.main(["--evaluate", str(path), "--min-line", "70", "--min-branch", "80"]) == cg.EXIT_PASS


def test_function_metric_absent_fails_a_gated_run(tmp_path):
    payload = summary()
    del payload["function_total"]
    path = write_summary(tmp_path, payload)
    assert cg.main(["--evaluate", str(path), "--min-function", "50"]) == cg.EXIT_UNMEASURED


# ---- the gcovr command line ----------------------------------------------------------------------


def test_gcovr_argv_ignores_every_parse_error_class(tmp_path):
    """Both classes must be survivable. gcovr's option takes ONE value, so it has to be `all`:
    naming only suspicious_hits is what left negative_hits fatal for three weeks (#1128)."""
    args = cg.parse_args(["--filter", "engine/", "--gcov-exclude", ".*/_deps/.*", "--exclude-throw-branches"])
    argv = cg.build_gcovr_argv(args, tmp_path / "s.json")
    assert "--gcov-ignore-parse-errors" in argv
    assert argv[argv.index("--gcov-ignore-parse-errors") + 1] == "all"
    assert argv.count("--filter") == 1 and "engine/" in argv
    assert "--exclude-throw-branches" in argv
    assert "--json-summary-pretty" in argv
    # No --fail-under-*: thresholds are this tool's job, precisely so a threshold verdict and a
    # measurement failure cannot share an exit code again.
    assert not any(a.startswith("--fail-under") for a in argv)


def test_gcovr_runs_once_for_all_thresholds(tmp_path):
    counter = tmp_path / "calls"
    payload = json.dumps(summary(line=(760, 1000), branch=(840, 1000))).replace("'", "")
    gcovr = stub_gcovr(tmp_path, f"""
echo x >> {counter}
out=""
while [ $# -gt 0 ]; do if [ "$1" = "-o" ]; then out="$2"; fi; shift; done
cat > "$out" <<'JSON'
{payload}
JSON
""")
    rc = cg.main(["--gcovr", str(gcovr), "--filter", "engine/", "--min-line", "70", "--min-branch", "80",
                  "--summary-json", str(tmp_path / "s.json")])
    assert rc == cg.EXIT_PASS
    assert counter.read_text().count("x") == 1


# ---- the step summary ------------------------------------------------------------------------------


def test_step_summary_records_the_verdict(tmp_path, monkeypatch):
    step = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(step))
    path = write_summary(tmp_path, summary(branch=(700, 1000)))
    assert cg.main(["--evaluate", str(path), "--min-branch", "80", "--min-line", "70"]) == cg.EXIT_THRESHOLD
    text = step.read_text(encoding="utf-8")
    assert "Coverage gate" in text and "70.00%" in text
    # The worst files are listed for the metric that failed, so the run says where to go next.
    assert "TerrainStreamer.cpp" in text


def test_step_summary_for_a_measurement_failure_quotes_no_number(tmp_path, monkeypatch):
    step = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(step))
    path = write_summary(tmp_path, summary(files=[]))
    assert cg.main(["--evaluate", str(path), "--min-branch", "80", "--min-line", "70"]) == cg.EXIT_UNMEASURED
    text = step.read_text(encoding="utf-8")
    assert "Not measured" in text
    # It may quote gcovr's bogus 0.0% to explain why that number is meaningless, but it must not
    # render a verdict table or claim the coverage was measured and found wanting.
    assert "below threshold" not in text
    assert "| metric |" not in text


def test_unwritable_step_summary_does_not_change_the_verdict(tmp_path, monkeypatch):
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(tmp_path / "no-such-dir" / "summary.md"))
    path = write_summary(tmp_path, summary(line=(760, 1000), branch=(840, 1000)))
    assert cg.main(["--evaluate", str(path), "--min-line", "70", "--min-branch", "80"]) == cg.EXIT_PASS


@pytest.mark.skipif(os.name == "nt", reason="stub gcovr is a bash script")
def test_end_to_end_threshold_miss_through_the_stub(tmp_path, capsys):
    payload = json.dumps(summary(branch=(500, 1000)))
    gcovr = stub_gcovr(tmp_path, f"""
out=""
while [ $# -gt 0 ]; do if [ "$1" = "-o" ]; then out="$2"; fi; shift; done
cat > "$out" <<'JSON'
{payload}
JSON
""")
    rc = cg.main(["--gcovr", str(gcovr), "--filter", "engine/", "--min-branch", "80", "--min-line", "70",
                  "--summary-json", str(tmp_path / "s.json")])
    assert rc == cg.EXIT_THRESHOLD
    assert "branch 50.00% < 80.0%" in capsys.readouterr().err
