# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/lint_test_names.py.

The linter's whole value is that it fires on the one thing that breaks (a test NAME) and stays
silent about the many places non-ASCII is correct and wanted (comments, log strings, assertion
text). Both halves are tested here, because a linter that cried wolf about prose would be turned
off within a week and the Windows failure would come back.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import lint_test_names as lint  # noqa: E402


# ---------------------------------------------------------------------------
# What must be caught
# ---------------------------------------------------------------------------


def test_em_dash_in_a_test_name_is_caught():
    # The literal case from #1145: ctest could not select this test on Windows, so it never ran
    # and was scored as a failure.
    src = 'TEST_CASE("AtcFacility: a landed flight is never retired — pinned pending #1149", "[atc]") {}'
    problems = lint.scan_source(src)
    assert len(problems) == 1
    line, macro, literal, bad = problems[0]
    assert line == 1
    assert macro == "TEST_CASE"
    assert bad == ["—"]
    assert "never retired" in literal


@pytest.mark.parametrize(
    "ch",
    ["—", "–", "‘", "’", "“", "”", "…", " ", "→"],
)
def test_every_character_a_writer_reaches_for_is_caught(ch):
    # These arrive by way of an editor's smart quotes or a paste from a document, which is exactly
    # how the original one got in.
    src = f'TEST_CASE("a name with {ch} in it", "[tag]") {{}}'
    assert lint.scan_source(src)


def test_a_non_ascii_tag_is_caught_too():
    # Tags are filter strings as well; `ctest -R` and Catch2's own tag selection hit the same wall.
    src = 'TEST_CASE("a perfectly ascii name", "[café]") {}'
    problems = lint.scan_source(src)
    assert len(problems) == 1
    assert problems[0][2] == "[café]"


@pytest.mark.parametrize(
    "macro",
    [
        "TEST_CASE",
        "SCENARIO",
        "TEMPLATE_TEST_CASE",
    ],
)
def test_every_registering_macro_is_scanned(macro):
    src = f'{macro}("bad — name", "[tag]") {{}}'
    assert lint.scan_source(src)


def test_a_fixture_macro_is_scanned_past_its_non_string_argument():
    # TEST_CASE_METHOD takes the fixture type first, so the name is not argument zero.
    src = 'TEST_CASE_METHOD(MyFixture, "bad — name", "[tag]") {}'
    problems = lint.scan_source(src)
    assert len(problems) == 1
    assert problems[0][2] == "bad — name"


def test_a_name_wrapped_across_lines_is_still_read():
    # clang-format wraps long names, and the offending character often lands on the second line.
    src = (
        'TEST_CASE("a very long name that clang-format decided to wrap — like this one",\n'
        '          "[tag]") {}'
    )
    problems = lint.scan_source(src)
    assert len(problems) == 1
    assert problems[0][0] == 1  # reported at the macro, where the reader will look


def test_the_offending_line_number_is_the_macro_not_the_file_start():
    src = "// a comment\n\n\nTEST_CASE(\"bad — name\", \"[tag]\") {}"
    assert lint.scan_source(src)[0][0] == 4


def test_several_offenders_in_one_name_are_all_reported():
    src = 'TEST_CASE("‘quoted’ — thing", "[tag]") {}'
    bad = lint.scan_source(src)[0][3]
    assert set(bad) == {"‘", "’", "—"}


# ---------------------------------------------------------------------------
# What must NOT be caught
# ---------------------------------------------------------------------------


def test_prose_around_the_test_is_left_alone():
    # This file's own comment style, and the codebase's. Flagging it would make the linter useless
    # noise and it would be disabled.
    src = (
        "// The runway is a mutex — releasing it is what this FSM exists for.\n"
        "//\n"
        "// A dash — an ellipsis … a quote “like this” all belong in prose.\n"
        'TEST_CASE("a clean ascii name", "[atc]") {\n'
        '    CHECK(message == "the pilot said “hello”");  // a UTF-8 assertion string\n'
        '    log.warn("café — not a test name");\n'
        "}\n"
    )
    assert lint.scan_source(src) == []


def test_a_plain_ascii_test_is_clean():
    src = 'TEST_CASE("AtcFacility: a landed flight is never retired (pinned pending #1149)", "[atc]") {}'
    assert lint.scan_source(src) == []


def test_a_function_that_merely_mentions_a_macro_name_is_not_a_test():
    # `describeTEST_CASE` and the like must not be picked up by a loose substring match.
    src = 'const char* describeTEST_CASEs = "a name with — in it";'
    assert lint.scan_source(src) == []


def test_escaped_quotes_inside_a_name_do_not_end_the_literal():
    src = 'TEST_CASE("a name with an escaped \\" quote — and a dash", "[tag]") {}'
    problems = lint.scan_source(src)
    assert len(problems) == 1
    assert "—" in problems[0][3]


def test_an_unbalanced_invocation_is_skipped_rather_than_crashing():
    # A file mid-edit, or one this parser does not understand. The compiler diagnoses it better.
    assert lint.scan_source('TEST_CASE("unterminated — name", "[tag]"') == []


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def test_the_character_is_named_so_it_can_be_found():
    # An invisible character is useless to report by appearance alone.
    assert lint.describe("—") == "U+2014 EM DASH"
    assert lint.describe(" ") == "U+00A0 NO-BREAK SPACE"


def test_a_suggestion_is_offered_for_the_common_cases():
    assert "—" in lint.SUGGESTIONS
    assert "’" in lint.SUGGESTIONS


def test_lint_file_reports_path_line_and_hint(tmp_path):
    f = tmp_path / "test_thing.cpp"
    f.write_text('TEST_CASE("bad — name", "[tag]") {}', encoding="utf-8")
    out = lint.lint_file(f)
    assert len(out) == 1
    assert "test_thing.cpp:1" in out[0]
    assert "EM DASH" in out[0]
    assert "-- (or wrap the clause in parentheses)" in out[0]


def test_main_exits_nonzero_on_a_violation_and_zero_when_clean(tmp_path, capsys):
    bad = tmp_path / "test_bad.cpp"
    bad.write_text('TEST_CASE("bad — name", "[t]") {}', encoding="utf-8")
    assert lint.main(["lint_test_names.py", str(bad)]) == 1

    good = tmp_path / "test_good.cpp"
    good.write_text('TEST_CASE("good name", "[t]") {}', encoding="utf-8")
    assert lint.main(["lint_test_names.py", str(good)]) == 0


def test_the_whole_tests_directory_is_clean():
    # The regression guard proper: the tree this linter defends must satisfy it.
    problems = []
    for f in lint.default_paths():
        problems += lint.lint_file(f)
    assert problems == [], "\n".join(problems)
