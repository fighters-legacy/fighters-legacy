#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject non-ASCII characters in Catch2 test names and tags.

`catch_discover_tests()` registers one ctest test per Catch2 test case, using the case's NAME as
the ctest name. To run it, ctest passes that name straight back to the test binary as a filter
argument. On Windows that round trip goes through a code page that is not UTF-8, so a non-ASCII
character arrives mangled, the filter matches nothing, and ctest reports:

    Filters: "AtcFacility: a landed flight is never retired ? pinned pending #1149"
    No test cases matched
    ***Failed

The test never ran. It is scored as a FAILURE for a case that passes everywhere it is executed,
and the Linux and macOS legs stay green — so the diagnosis costs a full CI round trip, and the
failure text points at ATC rather than at an em dash. That happened on #1145 (2026-08-07).

Test names are the one place in this codebase where non-ASCII is genuinely unsafe. Prose is not:
comments, log strings, docs and localisation data are UTF-8 throughout and stay that way. This
linter therefore reads ONLY the string literal arguments of the Catch2 registration macros, and
deliberately ignores everything else in the file.

Usage:  python3 tools/lint_test_names.py [paths...]
Exit 0 when clean, 1 when a violation is found.
"""

from __future__ import annotations

import re
import sys
import unicodedata
from pathlib import Path

# The Catch2 macros whose first string argument becomes a ctest test name. TEST_CASE_METHOD and
# TEMPLATE_TEST_CASE take a fixture/type first, so every string literal in the invocation is
# checked rather than a fixed argument index -- the tag string has the same filter problem.
MACROS = (
    "TEST_CASE",
    "TEST_CASE_METHOD",
    "TEMPLATE_TEST_CASE",
    "TEMPLATE_TEST_CASE_METHOD",
    "SCENARIO",
    "SCENARIO_METHOD",
)

_MACRO_RE = re.compile(r"\b(" + "|".join(MACROS) + r")\s*\(")

# A C++ string literal, tolerating escaped quotes. Raw string literals (R"(...)") are not used for
# test names anywhere in this tree and are not matched.
_STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# Substitutions that read the same to a human. Anything not listed is reported without a
# suggestion rather than guessed at.
SUGGESTIONS = {
    "—": "-- (or wrap the clause in parentheses)",  # em dash
    "–": "-",  # en dash
    "‘": "'",  # left single quote
    "’": "'",  # right single quote
    "“": '"',  # left double quote
    "”": '"',  # right double quote
    "…": "...",  # ellipsis
    " ": "a plain space",  # non-breaking space
    "→": "->",  # rightwards arrow
    "×": "x",  # multiplication sign
    "≥": ">=",
    "≤": "<=",
}


def describe(ch: str) -> str:
    """Name one offending character the way a person would look it up."""
    try:
        name = unicodedata.name(ch)
    except ValueError:  # unnamed control character
        name = "unnamed control character"
    return f"U+{ord(ch):04X} {name}"


def find_invocations(text: str) -> list[tuple[int, str, list[str]]]:
    """Return [(line_number, macro, [string literals])] for every registration macro.

    Scans from the macro's opening parenthesis to its matching close, so an invocation whose name
    is wrapped across lines (clang-format does this often) is read whole.
    """
    out: list[tuple[int, str, list[str]]] = []
    for m in _MACRO_RE.finditer(text):
        macro = m.group(1)
        depth = 0
        i = m.end() - 1  # at the '('
        end = None
        in_string = False
        escaped = False
        while i < len(text):
            c = text[i]
            if in_string:
                if escaped:
                    escaped = False
                elif c == "\\":
                    escaped = True
                elif c == '"':
                    in_string = False
            elif c == '"':
                in_string = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    end = i
                    break
            i += 1
        if end is None:
            continue  # unbalanced; the compiler will say so more usefully than we can
        args = text[m.end() : end]
        out.append((text.count("\n", 0, m.start()) + 1, macro, _STRING_RE.findall(args)))
    return out


def scan_source(text: str) -> list[tuple[int, str, str, list[str]]]:
    """Return [(line, macro, literal, [offending characters])] for one translation unit."""
    problems = []
    for line, macro, literals in find_invocations(text):
        for lit in literals:
            bad = sorted({ch for ch in lit if ord(ch) > 127})
            if bad:
                problems.append((line, macro, lit, bad))
    return problems


def lint_file(path: Path) -> list[str]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        return [f"{path}: could not read: {exc}"]
    out = []
    for line, macro, literal, bad in scan_source(text):
        chars = ", ".join(f"{describe(c)!s} ({c})" for c in bad)
        msg = f'{path}:{line}: {macro} name/tag contains non-ASCII: {chars}\n    in: "{literal}"'
        hints = [f"      {describe(c)} -> use {SUGGESTIONS[c]}" for c in bad if c in SUGGESTIONS]
        if hints:
            msg += "\n" + "\n".join(hints)
        out.append(msg)
    return out


def default_paths() -> list[Path]:
    root = Path(__file__).resolve().parent.parent
    return sorted((root / "tests").glob("*.cpp"))


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv[1:]] or default_paths()
    problems = []
    for p in paths:
        if p.is_dir():
            for f in sorted(p.glob("**/*.cpp")):
                problems += lint_file(f)
        elif p.exists():
            problems += lint_file(p)
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    if problems:
        print(
            f"\n{len(problems)} violation(s). ctest passes a test's name back to the binary as a "
            f"filter; a non-ASCII character does not survive that round trip on Windows, so the "
            f"test silently never runs and is scored as a failure. Comments and log strings are "
            f"unaffected -- only the name and tag literals are read.",
            file=sys.stderr,
        )
        return 1
    print(f"lint_test_names: {len(paths)} file(s) clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
