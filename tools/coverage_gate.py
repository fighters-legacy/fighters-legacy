#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Coverage gate: run gcovr once, then fail with a message that says WHICH thing went wrong.

The defect this exists to prevent (#1128) is not "coverage fell": it is a gate that cannot tell
"the code is under-covered" from "no number was produced". `gcovr --fail-under-branch 80` conflates
them — a parse abort, a filter that matches nothing, and a genuine regression all surface as the
same red X under a step named after the threshold. The Coverage workflow was red for three weeks
because `gcovr` was aborting on a gcov bug while *parsing*, and every reader of the run list saw
"coverage below 80%".

So this tool separates the two outcomes, by construction:

    exit 0  PASS            — a measured percentage at or above every threshold
    exit 1  THRESHOLD MISS  — a measured percentage below a threshold; the message names both
    exit 2  NOT MEASURED    — no percentage exists; the message says so and never quotes a number

"No percentage exists" is deliberately broad, because every one of these has been observed to
produce a plausible-looking 0.0% or a silent pass somewhere:

  * gcovr exited non-zero (the parse abort);
  * the summary file is missing or is not JSON;
  * the summary covers zero files (filters matched nothing — gcovr exits 0 and reports 0.0%);
  * a metric's denominator is zero (no branches/lines at all — 0/0 is not a percentage).

Usage (the workflow passes the same filters the report is built from):

    tools/coverage_gate.py --filter 'engine/' --gcov-exclude '.*/_deps/.*' \\
        --exclude-throw-branches --min-branch 80 --min-line 70 \\
        --summary-json coverage-summary.json

    tools/coverage_gate.py --evaluate coverage-summary.json --min-branch 80 --min-line 70
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "common"))
from gha import append_step_summary  # noqa: E402  (the one guarded step-summary writer, #1242)

EXIT_PASS = 0
EXIT_THRESHOLD = 1
EXIT_UNMEASURED = 2

# gcovr's parse-error classes are a single-value option (nargs="?"), so the two classes we have
# actually hit — suspicious_hits (the original mitigation) and negative_hits (#1128) — cannot both
# be named. `all` is the only spelling that survives both. Each skipped line is still reported on
# stderr, which this tool passes through, so the parse damage stays visible; what changes is that a
# gcov quirk no longer decides whether coverage gets measured at all.
GCOV_IGNORE_PARSE_ERRORS = "all"


class MeasurementFailure(Exception):
    """No coverage percentage was produced. Never carries a percentage — there isn't one."""


@dataclass(frozen=True)
class Metric:
    name: str
    covered: int
    total: int
    minimum: float | None

    @property
    def percent(self) -> float:
        return 100.0 * self.covered / self.total

    @property
    def passed(self) -> bool:
        return self.minimum is None or self.percent >= self.minimum


# ---- gcovr invocation ---------------------------------------------------------------------------


def build_gcovr_argv(args: argparse.Namespace, summary_path: Path) -> list[str]:
    """The single gcovr call. One invocation, one parse, one set of numbers for every threshold."""
    argv = [args.gcovr, "--root", args.root]
    for f in args.filter:
        argv += ["--filter", f]
    for e in args.gcov_exclude:
        argv += ["--gcov-exclude", e]
    if args.exclude_throw_branches:
        argv.append("--exclude-throw-branches")
    argv += ["--gcov-ignore-errors", "all"]
    argv += ["--gcov-ignore-parse-errors", GCOV_IGNORE_PARSE_ERRORS]
    argv += list(args.gcovr_arg)
    argv += ["--json-summary-pretty", "-o", str(summary_path)]
    return argv


def run_gcovr(argv: list[str], timeout_s: int) -> None:
    """Run gcovr, echoing its output. Raises MeasurementFailure on anything but a clean exit."""
    if shutil.which(argv[0]) is None and not Path(argv[0]).exists():
        raise MeasurementFailure(f"gcovr not found on PATH as {argv[0]!r}")
    print("+ " + " ".join(argv), file=sys.stderr, flush=True)
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout_s, check=False)
    except subprocess.TimeoutExpired:
        raise MeasurementFailure(f"gcovr did not finish within {timeout_s}s") from None
    except OSError as exc:
        raise MeasurementFailure(f"gcovr could not be executed: {exc}") from None
    # gcovr's own diagnostics are the most useful thing in a failing run — never swallow them.
    if proc.stdout:
        sys.stderr.write(proc.stdout)
    if proc.stderr:
        sys.stderr.write(proc.stderr)
    sys.stderr.flush()
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()[-3:]
        detail = ("; ".join(line.strip() for line in tail)) if tail else "no diagnostics"
        raise MeasurementFailure(f"gcovr exited {proc.returncode} before producing a summary ({detail})")


def load_summary(path: Path) -> dict:
    """Read gcovr's JSON summary, rejecting every shape that has no percentage in it."""
    if not path.is_file():
        raise MeasurementFailure(f"gcovr wrote no summary file at {path}")
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise MeasurementFailure(f"summary file {path} could not be read: {exc}") from None
    try:
        summary = json.loads(text)
    except json.JSONDecodeError as exc:
        raise MeasurementFailure(f"summary file {path} is not valid JSON: {exc}") from None
    if not isinstance(summary, dict):
        raise MeasurementFailure(f"summary file {path} is not a JSON object")
    files = summary.get("files")
    if not isinstance(files, list) or not files:
        # gcovr exits 0 here and reports 0.0% for everything. Gating on that number would fail
        # with "coverage is 0%" when the truth is that the filters matched nothing at all.
        raise MeasurementFailure(
            "the coverage report covers no files — the filters matched nothing, "
            "so the 0.0% gcovr reports is not a measurement"
        )
    return summary


def metric_from_summary(summary: dict, name: str, minimum: float | None) -> Metric:
    covered = summary.get(f"{name}_covered")
    total = summary.get(f"{name}_total")
    if not isinstance(covered, int) or not isinstance(total, int):
        raise MeasurementFailure(f"summary has no usable {name}_covered/{name}_total counts")
    if total <= 0:
        raise MeasurementFailure(f"summary reports zero {name} entities in scope — 0/0 is not a percentage")
    if covered < 0 or covered > total:
        raise MeasurementFailure(f"summary reports {covered} of {total} {name} entities covered, which is impossible")
    return Metric(name=name, covered=covered, total=total, minimum=minimum)


# ---- reporting ----------------------------------------------------------------------------------


def metric_line(m: Metric) -> str:
    limit = "no threshold" if m.minimum is None else f"threshold {m.minimum:.1f}%"
    verdict = "OK" if m.passed else "BELOW"
    return f"{m.name:<8} {m.percent:6.2f}%  ({m.covered}/{m.total})  {limit}  {verdict}"


def worst_files(summary: dict, key: str, limit: int) -> list[tuple[str, float, int, int]]:
    """The lowest-covered files with a non-zero denominator — what to actually go and test."""
    rows = []
    for entry in summary.get("files", []):
        total = entry.get(f"{key}_total")
        covered = entry.get(f"{key}_covered")
        if not isinstance(total, int) or not isinstance(covered, int) or total <= 0:
            continue
        rows.append((str(entry.get("filename", "?")), 100.0 * covered / total, covered, total))
    rows.sort(key=lambda r: (r[1], r[0]))
    return rows[:limit]


def write_step_summary(lines: list[str]) -> None:
    append_step_summary("\n".join(lines))


def annotate(title: str, message: str) -> None:
    """A GitHub Actions error annotation; harmless noise anywhere else."""
    flat = message.replace("\n", " ")
    print(f"::error title={title}::{flat}")


# ---- main ---------------------------------------------------------------------------------------


def report(summary: dict, metrics: list[Metric], scope: str) -> int:
    failed = [m for m in metrics if not m.passed]
    print(f"coverage measured over {len(summary.get('files', []))} file(s) in {scope}")
    for m in metrics:
        print("  " + metric_line(m))

    md = ["## Coverage gate", "", f"Scope: `{scope}` — {len(summary.get('files', []))} file(s) measured", "",
          "| metric | covered | total | measured | threshold | verdict |", "|---|---:|---:|---:|---:|---|"]
    for m in metrics:
        limit = "—" if m.minimum is None else f"{m.minimum:.1f}%"
        md.append(
            f"| {m.name} | {m.covered} | {m.total} | {m.percent:.2f}% | {limit} | "
            f"{'✅' if m.passed else '❌ below threshold'} |"
        )

    if failed:
        worst = worst_files(summary, failed[0].name, 10)
        if worst:
            md += ["", f"Lowest {failed[0].name} coverage:", "", "| file | measured | covered | total |",
                   "|---|---:|---:|---:|"]
            md += [f"| `{name}` | {pct:.1f}% | {cov} | {tot} |" for name, pct, cov, tot in worst]
        detail = "; ".join(f"{m.name} {m.percent:.2f}% < {m.minimum:.1f}%" for m in failed)
        md += ["", f"**Below threshold:** {detail}"]
        write_step_summary(md)
        annotate("Coverage below threshold", f"Measured {detail} over {scope}. The number was produced; it is too low.")
        print(f"\nCOVERAGE BELOW THRESHOLD: {detail}", file=sys.stderr)
        return EXIT_THRESHOLD

    write_step_summary(md)
    print("\ncoverage gate passed")
    return EXIT_PASS


def fail_unmeasured(reason: str, scope: str) -> int:
    message = (
        f"NO COVERAGE NUMBER WAS PRODUCED for {scope}: {reason}. "
        "This is a measurement failure, not a coverage regression — nothing was measured, "
        "so no percentage is being claimed either way."
    )
    annotate("Coverage could not be measured", message)
    write_step_summary(["## Coverage gate", "", f"❌ **Not measured** — {reason}", "",
                        "No percentage was produced, so no threshold verdict exists. "
                        "Fix the measurement before reading anything into this run."])
    print(f"\n{message}", file=sys.stderr)
    return EXIT_UNMEASURED


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Run gcovr once and gate on the result it actually produced.")
    p.add_argument("--filter", action="append", default=[], help="gcovr --filter (repeatable)")
    p.add_argument("--gcov-exclude", action="append", default=[], help="gcovr --gcov-exclude (repeatable)")
    p.add_argument("--exclude-throw-branches", action="store_true", help="pass gcovr --exclude-throw-branches")
    p.add_argument("--gcovr-arg", action="append", default=[], help="extra verbatim gcovr argument (repeatable)")
    p.add_argument("--root", default=".", help="gcovr --root (default: .)")
    p.add_argument("--gcovr", default="gcovr", help="gcovr executable (default: gcovr)")
    p.add_argument("--min-line", type=float, default=None, help="minimum line coverage percent")
    p.add_argument("--min-branch", type=float, default=None, help="minimum branch coverage percent")
    p.add_argument("--min-function", type=float, default=None, help="minimum function coverage percent")
    p.add_argument("--summary-json", default="coverage-summary.json", help="where to write gcovr's JSON summary")
    p.add_argument("--evaluate", metavar="FILE", default=None,
                   help="gate an existing gcovr JSON summary instead of running gcovr")
    p.add_argument("--timeout", type=int, default=1800, help="seconds to allow gcovr (default: 1800)")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    scope = ", ".join(args.filter) if args.filter else args.root
    summary_path = Path(args.evaluate) if args.evaluate else Path(args.summary_json)

    try:
        if not args.evaluate:
            run_gcovr(build_gcovr_argv(args, summary_path), args.timeout)
        summary = load_summary(summary_path)
        # Line and branch are the gated metrics, so a broken denominator in either is a measurement
        # failure whether or not a threshold was asked for. Functions are reported when present and
        # gated only on request — a report with no function entities is odd, not unmeasurable.
        metrics = [
            metric_from_summary(summary, "line", args.min_line),
            metric_from_summary(summary, "branch", args.min_branch),
        ]
        try:
            metrics.append(metric_from_summary(summary, "function", args.min_function))
        except MeasurementFailure as exc:
            if args.min_function is not None:
                raise
            print(f"note: function coverage not reported ({exc})", file=sys.stderr)
    except MeasurementFailure as exc:
        return fail_unmeasured(str(exc), scope)

    return report(summary, metrics, scope)


if __name__ == "__main__":
    sys.exit(main())
