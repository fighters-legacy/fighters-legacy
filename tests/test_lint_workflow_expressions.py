# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/lint_workflow_expressions.py (pure logic, no GitHub needed)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import lint_workflow_expressions as lint  # noqa: E402


# --- classify -------------------------------------------------------------------------------

@pytest.mark.parametrize(
    "expr",
    [
        " inputs.mission ",                       # demo-videos.yml
        " github.event.inputs.mission ",
        " steps.cliff.outputs.content ",          # release.yml fallback notes
        " needs.build.outputs.version ",
        " github.head_ref ",                      # fork branch name is attacker-chosen
        " github.event.release.body ",            # the release-notes gate's own bug
        " github.event.issue.title ",
        " github.event.comment.body ",
        " github.event.head_commit.message ",
    ],
)
def test_free_text_is_rejected(expr):
    assert lint.classify(expr) == "free-text"


@pytest.mark.parametrize(
    "expr",
    [
        " matrix.preset ",
        " matrix.artifact ",
        " needs.fuzz-run.result ",
        " env.SOMETHING ",
        " secrets.TOKEN ",
        " github.token ",
        " github.sha ",
        " github.repository ",
        " github.base_ref ",                      # PR target branch, not attacker-chosen
        " github.event.pull_request.base.sha ",   # a hash living under a free-text object
        " github.event.pull_request.head.sha ",
        " github.event.before ",
        " hashFiles('cmake/dependencies.cmake') ",
    ],
)
def test_constrained_values_are_allowed(expr):
    assert lint.classify(expr) == "allowed"


def test_allowed_wins_over_free_text():
    """A SHA must not be condemned by the event object it happens to live under."""
    assert lint.classify(" github.event.pull_request.base.sha ") == "allowed"
    assert lint.classify(" github.event.pull_request.title ") == "free-text"


def test_unknown_fails_closed():
    """Anything unclassified is rejected: this is a security lint."""
    assert lint.classify(" github.some_future_context.value ") == "unknown"


# --- scan_run -------------------------------------------------------------------------------

def test_scan_run_ignores_allowed_expressions():
    assert scan("cmake --preset ${{ matrix.preset }}") == []


def scan(run):
    return lint.scan_run(run)


def test_scan_run_flags_free_text():
    found = scan('body="${{ steps.cliff.outputs.content }}"')
    assert [v for _, v in found] == ["free-text"]


def test_scan_run_flags_expression_inside_a_comment():
    """The exact shape of the release-notes-gate bug: Actions substitutes comments too."""
    run = "# do not interpolate ${{ github.event.release.body }} here\necho hi\n"
    assert [v for _, v in scan(run)] == ["free-text"]


def test_scan_run_handles_multiline_expression():
    assert [v for _, v in scan("x=${{\n  inputs.mission\n}}")] == ["free-text"]


def test_scan_run_empty_and_none():
    assert scan("") == []
    assert lint.scan_run(None) == []


# --- lint_file ------------------------------------------------------------------------------

def write(tmp_path, text):
    p = tmp_path / "wf.yml"
    p.write_text(text, encoding="utf-8")
    return p


def test_lint_file_clean_workflow(tmp_path):
    p = write(tmp_path, """
jobs:
  build:
    steps:
      - name: Configure
        run: cmake --preset ${{ matrix.preset }}
""")
    assert lint.lint_file(p) == []


def test_lint_file_reports_job_step_and_expression(tmp_path):
    p = write(tmp_path, """
jobs:
  release:
    steps:
      - name: Fallback release notes
        run: body="${{ steps.cliff.outputs.content }}"
""")
    problems = lint.lint_file(p)
    assert len(problems) == 1
    assert "release" in problems[0]
    assert "Fallback release notes" in problems[0]
    assert "steps.cliff.outputs.content" in problems[0]
    assert "env:" in problems[0]          # the fix is named in the message


def test_lint_file_covers_composite_actions(tmp_path):
    """Composite actions use `runs.steps`, not `jobs` — they must be scanned too."""
    p = write(tmp_path, """
runs:
  using: composite
  steps:
    - name: Install apt packages
      shell: bash
      run: if [ "${{ inputs.vulkan }}" = "true" ]; then echo yes; fi
""")
    problems = lint.lint_file(p)
    assert len(problems) == 1
    assert "inputs.vulkan" in problems[0]


def test_lint_file_ignores_non_run_contexts(tmp_path):
    """`if:`/`with:`/`env:` are Actions expression contexts, not shells — flagging them would
    condemn the very fix this linter asks for."""
    p = write(tmp_path, """
jobs:
  record:
    steps:
      - name: Attach
        if: ${{ inputs.release_tag != '' }}
        env:
          RELEASE_TAG: ${{ inputs.release_tag }}
        with:
          ref: ${{ inputs.ref }}
        run: gh release upload "$RELEASE_TAG" out/*.mp4
""")
    assert lint.lint_file(p) == []


def test_lint_file_tolerates_non_mapping_yaml(tmp_path):
    p = tmp_path / "list.yml"
    p.write_text("- just\n- a list\n", encoding="utf-8")
    assert lint.lint_file(p) == []


# --- the real tree --------------------------------------------------------------------------

def test_repository_workflows_are_clean():
    """The committed workflows must pass, or the gate is not enforceable."""
    problems = []
    for p in lint.default_paths():
        problems += lint.lint_file(p)
    assert problems == [], "\n".join(problems)
