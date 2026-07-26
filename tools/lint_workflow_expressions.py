#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject GitHub Actions expressions carrying free text inside `run:` blocks.

Actions substitutes ``${{ }}`` into a run block BEFORE the script is written to disk, so an
expression holding free text becomes shell *source* rather than data. That is a code-execution
vector, and three instances shipped in this repository before anyone noticed (2026-07-26):

  * ``release.yml`` spliced git-cliff output — built from commit messages and PR titles, i.e. text
    any contributor chooses — into a shell assignment.
  * ``release-notes-gate.yml`` referenced the release body inside a *comment* explaining why the
    body must never be interpolated. Comments are substituted too, so the body became script. It
    broke on the first release it guarded, when ``(#531, Epic #499)`` met bash as a syntax error.
  * ``demo-videos.yml`` passed five workflow_dispatch inputs straight into a command line.

Only ``run:`` is a shell. ``if:``, ``with:``, ``env:`` values, ``name:`` and ``runs-on:`` are
Actions expression contexts evaluated by the runner, and are deliberately NOT flagged — flagging
them would condemn the fix this linter asks for.

Usage:  python3 tools/lint_workflow_expressions.py [paths...]
Exit 0 when clean, 1 when a violation is found.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - exercised only on a machine without PyYAML
    print("lint_workflow_expressions: PyYAML is required", file=sys.stderr)
    raise SystemExit(2)

EXPR = re.compile(r"\$\{\{(.+?)\}\}", re.S)

# Values that are repo-controlled, enumerated, or structurally constrained (hex, booleans).
# CHECKED FIRST: `github.event.pull_request.base.sha` is a commit hash, not prose, even though it
# lives under an event object whose other fields are free text.
ALLOWED = (
    re.compile(r"^\s*matrix\."),
    re.compile(r"^\s*env\."),
    re.compile(r"^\s*secrets\."),
    re.compile(r"^\s*needs\.[\w-]+\.result\b"),
    re.compile(r"^\s*github\.(token|sha|repository|repository_owner|run_id|run_number|"
               r"workspace|server_url|ref_name|ref|event_name|actor|api_url|base_ref|"
               r"workflow|job|action|run_attempt)\b"),
    # Commit SHAs anywhere in the event payload; `before`/`after` on a push are SHAs too.
    re.compile(r"^\s*github\.event\..*\.sha\b"),
    re.compile(r"^\s*github\.event\.(before|after)\b"),
    re.compile(r"^\s*hashFiles\("),
)

# Contexts whose value is text a contributor, issue author or dispatcher can choose.
# NOTE: `github.head_ref` is here and `github.base_ref` is in ALLOWED — a fork's branch name is
# attacker-chosen, the PR's target branch is not.
FREE_TEXT = (
    re.compile(r"^\s*inputs\."),
    re.compile(r"^\s*github\.event\.inputs\."),
    re.compile(r"^\s*steps\.[\w-]+\.outputs\."),
    re.compile(r"^\s*needs\.[\w-]+\.outputs\."),
    re.compile(r"^\s*github\.head_ref\b"),
    re.compile(
        r"^\s*github\.event\..*\.(body|title|name|message|label|description|login|email)\b"
    ),
    re.compile(r"^\s*github\.event\.(comment|issue|pull_request|release|discussion)\b"),
)

HINT = (
    "pass it via `env:` and reference \"$VAR\" in the script — "
    "env values are delivered as environment variables, not spliced into the script text"
)


def classify(expr: str) -> str:
    """Return 'free-text', 'allowed', or 'unknown' for one expression's inner text.

    ALLOWED wins over FREE_TEXT so that a constrained field (a SHA) is not condemned by the
    event object it happens to live under. Anything matching neither list is 'unknown' and still
    fails: this is a security lint, so it fails closed, and adding a genuinely safe context to
    ALLOWED is a one-line, reviewable change.
    """
    if any(p.search(expr) for p in ALLOWED):
        return "allowed"
    if any(p.search(expr) for p in FREE_TEXT):
        return "free-text"
    return "unknown"


def scan_run(run: str) -> list[tuple[str, str]]:
    """Return [(expression, verdict)] for every expression in one run block."""
    out = []
    for m in EXPR.finditer(run or ""):
        inner = m.group(1)
        verdict = classify(inner)
        if verdict != "allowed":
            out.append((inner.strip(), verdict))
    return out


def iter_steps(doc: dict):
    """Yield (job_name, step) for every step in a workflow or composite action."""
    for job_name, job in (doc.get("jobs") or {}).items():
        for step in job.get("steps") or []:
            yield job_name, step
    runs = doc.get("runs") or {}          # composite actions
    if isinstance(runs, dict):
        for step in runs.get("steps") or []:
            yield "runs", step


def lint_file(path: Path) -> list[str]:
    try:
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        return [f"{path}: could not parse: {exc}"]
    if not isinstance(doc, dict):
        return []
    problems = []
    for job_name, step in iter_steps(doc):
        for expr, verdict in scan_run(step.get("run") or ""):
            label = step.get("name") or step.get("uses") or "<unnamed step>"
            kind = "free-text" if verdict == "free-text" else "unrecognised"
            problems.append(
                f"{path}: job '{job_name}', step '{label}': "
                f"{kind} expression ${{{{ {expr} }}}} inside a run block\n"
                f"    {HINT}"
            )
    return problems


def default_paths() -> list[Path]:
    root = Path(__file__).resolve().parent.parent
    paths = sorted((root / ".github" / "workflows").glob("*.y*ml"))
    paths += sorted((root / ".github" / "actions").glob("*/action.y*ml"))
    return paths


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv[1:]] or default_paths()
    problems = []
    for p in paths:
        if p.exists():
            problems += lint_file(p)
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    if problems:
        print(
            f"\n{len(problems)} violation(s). Actions substitutes ${{{{ }}}} before the script is "
            f"written, so free text becomes shell source. See issue #1034.",
            file=sys.stderr,
        )
        return 1
    print(f"lint_workflow_expressions: {len(paths)} file(s) clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
