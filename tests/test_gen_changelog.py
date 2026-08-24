# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for scripts/gen_changelog.py.

The part worth pinning is the *refusals*. This tool rewrites the top of a thousand-line file of
released history, and the way it fails badly is not by crashing — it is by producing a file that
still looks like a changelog with the history quietly gone. So every guard gets a test that proves
it trips, and the preservation check gets one that proves it catches a tail somebody tampered with
rather than merely agreeing with the string the tool itself just built.

git-cliff is never invoked here: the generation is one subprocess call, and the tests that matter
are about what happens to its output afterwards. The real invocation is covered by
`python3 scripts/gen_changelog.py vX.Y.Z --dry-run` when a release is cut.
"""

import datetime
from pathlib import Path

import pytest

from conftest import load_tool

gc = load_tool("gen_changelog", "scripts", "gen_changelog.py")

TODAY = datetime.date.today().isoformat()

HEADER = (
    "# Changelog\n"
    "\n"
    "All notable changes to this project will be documented in this file.\n"
    "\n"
    "Entries are **generated from conventional-commit subjects** by git-cliff.\n"
    "\n"
)

RELEASED = (
    "## [0.3.13] - 2026-07-28\n"
    "\n"
    "### Added\n"
    "\n"
    "- **server**: MCP surface (#601)\n"
    "\n"
    "## [0.3.12] - 2026-07-27\n"
    "\n"
    "### Fixed\n"
    "\n"
    "- **engine**: an older thing (#500)\n"
)

CHANGELOG = HEADER + RELEASED

SECTION = (
    f"## [0.3.14] - {TODAY}\n"
    "\n"
    "### Added\n"
    "\n"
    "- **game**: unlimited per-action bindings across multiple input devices (#1062)\n"
    "\n"
    "### Fixed\n"
    "\n"
    "- **game**: degrade to silent audio instead of refusing to start (#1122)\n"
)

CMAKELISTS = (
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(fighters-legacy VERSION 0.3.13 LANGUAGES C CXX)\n"
    "\n"
    "# A comment mentioning VERSION 1.2.3 that must not be rewritten.\n"
)


# ---- version parsing -----------------------------------------------------------------------------


def test_parse_version_strips_the_tag_prefix():
    assert gc.parse_version("v0.3.14") == "0.3.14"


@pytest.mark.parametrize("bad", ["0.3.14", "v0.3", "v0.3.14-rc1", "release-0.3.14", ""])
def test_parse_version_rejects_anything_else(bad):
    with pytest.raises(gc.GenError):
        gc.parse_version(bad)


# ---- G1 / G2 / G6: validating git-cliff's output --------------------------------------------------


def test_generated_section_accepted():
    gc.check_generated(SECTION, "0.3.14", TODAY)


def test_g1_refuses_a_section_with_no_entries():
    """A range of only ci/chore/build commits renders a heading and nothing under it."""
    empty = f"## [0.3.14] - {TODAY}\n"
    with pytest.raises(gc.GenError, match="no entries"):
        gc.check_generated(empty, "0.3.14", TODAY)


def test_g2_refuses_the_wrong_version():
    with pytest.raises(gc.GenError, match=r"expected '## \[0\.4\.0\]"):
        gc.check_generated(SECTION, "0.4.0", TODAY)


def test_g2_refuses_the_wrong_date():
    """tag-release.sh refuses to tag when the section date is not the tag date."""
    stale = SECTION.replace(TODAY, "2020-01-01")
    with pytest.raises(gc.GenError, match="generated heading"):
        gc.check_generated(stale, "0.3.14", TODAY)


def test_g6_refuses_a_reintroduced_unreleased_heading():
    """--tag was not honoured. That heading is what made every pair of open PRs conflict."""
    unreleased = SECTION.replace(f"## [0.3.14] - {TODAY}", "## [Unreleased]")
    with pytest.raises(gc.GenError, match="Unreleased"):
        gc.check_generated(unreleased, "0.3.14", TODAY)


# ---- G3 / G4: splicing ----------------------------------------------------------------------------


def test_splice_inserts_above_the_newest_section():
    result = gc.splice_section(CHANGELOG, SECTION, "0.3.14")
    assert result.startswith(HEADER)
    assert result.endswith(RELEASED)
    assert result == HEADER + SECTION + "\n" + RELEASED


def test_splice_leaves_the_released_sections_byte_identical():
    result = gc.splice_section(CHANGELOG, SECTION, "0.3.14")
    assert result[result.index("## [0.3.13]") :] == CHANGELOG[CHANGELOG.index("## [0.3.13]") :]


def test_splice_leaves_the_header_byte_identical():
    result = gc.splice_section(CHANGELOG, SECTION, "0.3.14")
    assert result[: len(HEADER)] == HEADER


def test_splice_normalizes_the_gap_to_one_blank_line():
    """git-cliff's trailing whitespace varies with the last group; the file's spacing must not."""
    for trailing in ("", "\n", "\n\n\n"):
        result = gc.splice_section(CHANGELOG, SECTION.rstrip("\n") + trailing, "0.3.14")
        assert "(#1122)\n\n## [0.3.13]" in result


def test_g3_refuses_a_version_that_is_already_in_the_file():
    with pytest.raises(gc.GenError, match=r"already has a \[0\.3\.13\] section"):
        gc.splice_section(CHANGELOG, SECTION.replace("0.3.14", "0.3.13"), "0.3.13")


def test_g3_makes_a_second_run_fail_instead_of_duplicating():
    once = gc.splice_section(CHANGELOG, SECTION, "0.3.14")
    with pytest.raises(gc.GenError, match="already has"):
        gc.splice_section(once, SECTION, "0.3.14")


def test_g4_refuses_a_file_with_no_section_anchor():
    with pytest.raises(gc.GenError, match="no '## \\[' section heading"):
        gc.splice_section(HEADER, SECTION, "0.3.14")


# ---- G5: preservation, re-derived independently ---------------------------------------------------


def test_g5_accepts_a_correct_splice():
    gc.assert_preserved(CHANGELOG, gc.splice_section(CHANGELOG, SECTION, "0.3.14"), "0.3.14")


def test_g5_catches_a_tampered_released_section():
    """The point of the guard: it must not simply agree with the string the tool built."""
    tampered = gc.splice_section(CHANGELOG, SECTION, "0.3.14").replace("MCP surface", "MCP surfaces")
    with pytest.raises(gc.GenError, match="not byte-identical"):
        gc.assert_preserved(CHANGELOG, tampered, "0.3.14")


def test_g5_catches_a_dropped_released_section():
    truncated = gc.splice_section(CHANGELOG, SECTION, "0.3.14").replace(
        "## [0.3.12] - 2026-07-27\n\n### Fixed\n\n- **engine**: an older thing (#500)\n", ""
    )
    with pytest.raises(gc.GenError, match="not byte-identical"):
        gc.assert_preserved(CHANGELOG, truncated, "0.3.14")


def test_g5_catches_a_changed_header():
    moved = gc.splice_section(CHANGELOG, SECTION, "0.3.14").replace("# Changelog", "# Change log")
    with pytest.raises(gc.GenError, match="header changed"):
        gc.assert_preserved(CHANGELOG, moved, "0.3.14")


def test_g5_catches_the_new_section_not_being_on_top():
    reordered = HEADER + RELEASED + SECTION
    with pytest.raises(gc.GenError, match=r"top section .* is not \[0\.3\.14\]"):
        gc.assert_preserved(CHANGELOG, reordered, "0.3.14")


# ---- line endings ---------------------------------------------------------------------------------


def test_a_crlf_file_stays_crlf():
    """A Windows checkout must not come back wholly rewritten as a whitespace-only diff."""
    crlf = CHANGELOG.replace("\n", "\r\n")
    result = gc.splice_section(crlf, SECTION, "0.3.14")
    assert "\r\n" in result
    assert "\n" not in result.replace("\r\n", "")
    gc.assert_preserved(crlf, result, "0.3.14")


def test_an_lf_file_stays_lf_even_when_git_cliff_emits_crlf():
    result = gc.splice_section(CHANGELOG, SECTION.replace("\n", "\r\n"), "0.3.14")
    assert "\r" not in result


@pytest.mark.parametrize("text", [CHANGELOG, CHANGELOG.replace("\n", "\r\n")])
def test_read_write_round_trip_is_byte_exact(tmp_path, text):
    """Through real disk I/O, which is where the newline handling actually has to hold.

    `Path.read_text(newline=...)` needs Python 3.13 and CI's runner is older, so `read_file` /
    `write_file` use `open()` directly. This is the test that would have caught that locally.
    """
    path = tmp_path / "CHANGELOG.md"
    gc.write_file(path, text)
    assert gc.read_file(path) == text
    assert path.read_bytes() == text.encode("utf-8")


def test_the_trailing_newline_is_preserved():
    assert gc.splice_section(CHANGELOG, SECTION, "0.3.14").endswith("(#500)\n")
    no_trailing = CHANGELOG.rstrip("\n")
    assert not gc.splice_section(no_trailing, SECTION, "0.3.14").endswith("\n")


# ---- CMakeLists version bump ----------------------------------------------------------------------


def test_bump_rewrites_only_the_project_line():
    result = gc.bump_cmake_version(CMAKELISTS, "0.3.14")
    assert "project(fighters-legacy VERSION 0.3.14 LANGUAGES C CXX)" in result
    assert "VERSION 1.2.3 that must not be rewritten" in result
    assert "cmake_minimum_required(VERSION 3.25)" in result


def test_bump_refuses_when_the_project_line_is_absent():
    with pytest.raises(gc.GenError, match="found 0"):
        gc.bump_cmake_version("project(something-else VERSION 1.0.0)\n", "0.3.14")


def test_bump_refuses_when_the_project_line_is_duplicated():
    with pytest.raises(gc.GenError, match="found 2"):
        gc.bump_cmake_version(CMAKELISTS + CMAKELISTS, "0.3.14")


# ---- the driver, with git-cliff stubbed out --------------------------------------------------------


@pytest.fixture
def repo(tmp_path, monkeypatch):
    gc.write_file(tmp_path / "CHANGELOG.md", CHANGELOG)
    gc.write_file(tmp_path / "CMakeLists.txt", CMAKELISTS)
    monkeypatch.setattr(gc, "resolve_git_cliff", lambda: ["git-cliff-stub"])
    monkeypatch.setattr(gc, "run_git_cliff", lambda argv, tag, root: SECTION)
    return tmp_path


def test_main_writes_both_files(repo):
    assert gc.main(["v0.3.14", "--repo-root", str(repo)]) == 0
    assert gc.read_file(repo / "CHANGELOG.md") == HEADER + SECTION + "\n" + RELEASED
    assert "VERSION 0.3.14" in gc.read_file(repo / "CMakeLists.txt")


def test_main_dry_run_writes_nothing(repo, capsys):
    before = (repo / "CHANGELOG.md").read_bytes(), (repo / "CMakeLists.txt").read_bytes()
    assert gc.main(["v0.3.14", "--dry-run", "--repo-root", str(repo)]) == 0
    assert (repo / "CHANGELOG.md").read_bytes() == before[0]
    assert (repo / "CMakeLists.txt").read_bytes() == before[1]
    assert "- **game**: degrade to silent audio" in capsys.readouterr().out


def test_main_reports_a_tripped_guard_and_leaves_the_files_alone(repo, capsys, monkeypatch):
    monkeypatch.setattr(gc, "run_git_cliff", lambda argv, tag, root: f"## [0.3.14] - {TODAY}\n")
    before = (repo / "CHANGELOG.md").read_bytes()
    assert gc.main(["v0.3.14", "--repo-root", str(repo)]) == 1
    assert "no entries" in capsys.readouterr().err
    assert (repo / "CHANGELOG.md").read_bytes() == before


def test_main_rejects_a_malformed_version(repo, capsys):
    assert gc.main(["0.3.14", "--repo-root", str(repo)]) == 1
    assert "vMAJOR.MINOR.PATCH" in capsys.readouterr().err
