# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Behavioural tests for .github/workflows/auto-close-epics.yml (#1384).

The workflow closes a parent issue when its last sub-issue closes. Its predicate matched the `epic`
label OR the "Epic" issue TYPE — and a phase-gate release Task is typed Epic, so #729 (v0.4.0) was
auto-closed at 2026-09-01T01:54:44Z with no tag and no release. That is not cosmetic: the release
task is the interlock that keeps a milestone open until the release ships, and M4.0 was left showing
1 open issue while its release did not exist.

It will not be exercised again until the next phase gate (#730-#735), so the guard is tested here
rather than reviewed. The step's shell is extracted from the workflow verbatim and run against a
stub `gh` — the real decision logic, not a re-implementation of it. `gh api graphql --jq` is stubbed
to emit the already-filtered parent object, which is what the real invocation's `--jq` yields.
"""

import json
import os
import shutil
import subprocess
import textwrap
from pathlib import Path

import pytest

WORKFLOW = Path(__file__).resolve().parent.parent / ".github" / "workflows" / "auto-close-epics.yml"

pytestmark = pytest.mark.skipif(shutil.which("jq") is None, reason="the workflow step needs jq")


def _extract_run_script(path):
    """Return the single `run: |` block in the workflow, dedented.

    Deliberately textual rather than yaml.safe_load: PyYAML is not a declared dependency of the
    python-tools CI job, and a test that silently skips is exactly the failure this file exists to
    prevent. Asserts there is exactly one block, so the extractor cannot quietly drift onto the
    wrong step.
    """
    lines = path.read_text(encoding="utf-8").splitlines()
    starts = [i for i, ln in enumerate(lines) if ln.rstrip().endswith("run: |")]
    assert len(starts) == 1, f"expected one `run: |` block in {path.name}, found {len(starts)}"
    start = starts[0]
    indent = len(lines[start]) - len(lines[start].lstrip())
    body = []
    for ln in lines[start + 1:]:
        if ln.strip() and (len(ln) - len(ln.lstrip())) <= indent:
            break
        body.append(ln)
    return textwrap.dedent("\n".join(body))


def _parent(*, number=729, state="OPEN", issue_type="Epic", labels=(), total=2, completed=2):
    return {
        "number": number,
        "state": state,
        "issueType": {"name": issue_type} if issue_type else None,
        "labels": {"nodes": [{"name": n} for n in labels]},
        "subIssuesSummary": {"total": total, "completed": completed},
    }


def _run_workflow_step(tmp_path, parent):
    """Run the extracted step with a stub `gh` on PATH. Returns (stdout, gh invocations)."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    log = tmp_path / "gh.log"
    (bin_dir / "gh").write_text(textwrap.dedent("""\
        #!/usr/bin/env python3
        import os, sys
        with open(os.environ["FAKE_GH_LOG"], "a") as f:
            f.write(" ".join(sys.argv[1:]) + "\\n")
        if sys.argv[1:3] == ["api", "graphql"]:
            sys.stdout.write(os.environ["FAKE_GH_PARENT"])
        """), encoding="utf-8")
    (bin_dir / "gh").chmod(0o755)

    env = dict(os.environ)
    env.update(PATH=f"{bin_dir}{os.pathsep}{env['PATH']}", GH_TOKEN="x", OWNER="o", REPO="r",
               NUMBER="1", FAKE_GH_LOG=str(log),
               FAKE_GH_PARENT=json.dumps(parent) if parent is not None else "null")
    proc = subprocess.run(["bash", "-c", _extract_run_script(WORKFLOW)], env=env,
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stderr
    calls = log.read_text(encoding="utf-8").splitlines() if log.exists() else []
    return proc.stdout, calls


def _closed(calls):
    return [c for c in calls if c.startswith("issue close")]


def test_release_task_is_never_auto_closed(tmp_path):
    """#1384: the exact shape of #729 — `release`-labelled, typed Epic, every sub-issue closed."""
    out, calls = _run_workflow_step(tmp_path, _parent(labels=["release"]))
    assert not _closed(calls)
    assert "closes by hand" in out


def test_release_task_is_not_closed_even_when_also_labelled_epic(tmp_path):
    """`release` is a veto, not a tie-break: it must win over every other way in."""
    _, calls = _run_workflow_step(tmp_path, _parent(labels=["release", "epic"]))
    assert not _closed(calls)


def test_epic_typed_parent_still_auto_closes(tmp_path):
    """#1146's self-closing behaviour must not regress — the type predicate still fires."""
    _, calls = _run_workflow_step(tmp_path, _parent(number=1063, labels=["ci"]))
    assert _closed(calls) == ["issue close 1063 --repo o/r --reason completed"]


def test_epic_labelled_parent_still_auto_closes(tmp_path):
    """The personal-edition predicate (label, no issue type) is untouched."""
    _, calls = _run_workflow_step(tmp_path, _parent(number=900, issue_type=None, labels=["epic"]))
    assert _closed(calls) == ["issue close 900 --repo o/r --reason completed"]


def test_parent_with_open_sub_issues_is_left_alone(tmp_path):
    _, calls = _run_workflow_step(tmp_path, _parent(number=901, labels=["epic"], completed=1))
    assert not _closed(calls)


def test_ordinary_parent_is_not_closed(tmp_path):
    """Neither labelled nor typed: not an epic, not this workflow's business."""
    _, calls = _run_workflow_step(tmp_path, _parent(number=902, issue_type="Task"))
    assert not _closed(calls)


def test_already_closed_parent_is_skipped(tmp_path):
    _, calls = _run_workflow_step(tmp_path, _parent(number=903, state="CLOSED", labels=["epic"]))
    assert not _closed(calls)


def test_sub_issue_without_a_parent_is_a_no_op(tmp_path):
    out, calls = _run_workflow_step(tmp_path, None)
    assert not _closed(calls)
    assert "no parent" in out
