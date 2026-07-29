# SPDX-FileCopyrightText: Contributors to Fighters Legacy
# SPDX-License-Identifier: GPL-3.0-or-later
"""Missions-as-integration-tests harness driver (#856).

Runs a mission headless to completion via `fl-server --mission <name> --mission-report <path>`, reads
the JSON outcome report, and asserts on it. This is what turns the mission format into the engine's
integration-test surface: a scripted mission runs with no clients, reports objectives/survivors/ticks,
and CI fails when the outcome regresses -- no human flying required.

fl-server's `--mission-report` mode is a deterministic fixed-step run (the overrun governor pinned off,
no wall-clock), so the outcome is reproducible run to run.

Only the pure assertion logic (`evaluate_report`) is unit-tested (tests/test_mission_test.py); the
subprocess launch is covered by the ctest that invokes this script against the ci-mission-pack fixture.
"""
import argparse
import json
import socket
import subprocess
import sys
import tempfile
from pathlib import Path


def free_port():
    """Grab an ephemeral TCP port so parallel ctest runs never collide on a fixed one."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def evaluate_report(report, expect_outcome=None, min_triggers=None, min_survivors=None, min_spawned=None):
    """Pure assertion over a decoded report dict. Returns a list of failure strings (empty = pass)."""
    failures = []
    if expect_outcome is not None and report.get("outcome") != expect_outcome:
        failures.append(f"outcome: expected {expect_outcome!r}, got {report.get('outcome')!r}")
    if min_triggers is not None and int(report.get("triggers_fired", 0)) < min_triggers:
        failures.append(f"triggers_fired: expected >= {min_triggers}, got {report.get('triggers_fired')}")
    if min_survivors is not None and int(report.get("live_entities", 0)) < min_survivors:
        failures.append(f"live_entities: expected >= {min_survivors}, got {report.get('live_entities')}")
    if min_spawned is not None and int(report.get("spawned_objects", 0)) < min_spawned:
        failures.append(f"spawned_objects: expected >= {min_spawned}, got {report.get('spawned_objects')}")
    # Entity soft cap (#1049). ALWAYS checked, with no opt-out: a run whose spawns were refused by the
    # cap is not a mission result at all, it is a truncated world, and every count above it is then
    # measuring the cap rather than the mission. Failing here names the real cause; leaving it out
    # would surface as a mystified "min_survivors: expected >= 2, got 1" on a mission that is fine.
    refusals = int(report.get("entity_cap_refusals", 0))
    if refusals > 0:
        failures.append(
            f"entity_cap_refusals: {refusals} spawn(s) refused by world.entity_soft_cap -- "
            "the run is truncated; raise or clear the cap before trusting any count above")
    return failures


def run_mission(server, assets, mission, report_path, timeout_s=60):
    """Launch fl-server headless; it runs the mission to completion, writes the report, and exits."""
    port = str(free_port())
    cmd = [server, port, "8", "--bind", "127.0.0.1", "--assets", assets,
           "--mission", mission, "--mission-report", report_path, "--transport", "enet"]
    proc = subprocess.run(cmd, timeout=timeout_s, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"fl-server exited {proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    return proc


def main(argv=None):
    ap = argparse.ArgumentParser(description="Run a mission headless and assert on its outcome (#856).")
    ap.add_argument("--server", required=True, help="path to the fl-server binary")
    ap.add_argument("--assets", required=True, help="content root holding mods/ (the mission's pack)")
    ap.add_argument("--mission", required=True, help="mission asset name to load")
    ap.add_argument("--expect-outcome", choices=["success", "failure", "incomplete"])
    ap.add_argument("--min-triggers", type=int)
    ap.add_argument("--min-survivors", type=int)
    ap.add_argument("--min-spawned", type=int)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args(argv)

    with tempfile.TemporaryDirectory() as tmp:
        report_path = str(Path(tmp) / "report.json")
        try:
            run_mission(args.server, args.assets, args.mission, report_path, args.timeout)
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            print(f"FAIL: {e}", file=sys.stderr)
            return 1
        with open(report_path, encoding="utf-8") as f:
            report = json.load(f)

    print(json.dumps(report, indent=2))
    failures = evaluate_report(report, args.expect_outcome, args.min_triggers, args.min_survivors, args.min_spawned)
    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        return 1
    print(f"PASS: mission {args.mission!r} -> {report.get('outcome')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
