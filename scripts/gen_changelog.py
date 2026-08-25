#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the `[X.Y.Z]` section of CHANGELOG.md from conventional-commit subjects (#1123).

Run by `scripts/cut-release.sh` on the release branch, so the generated section is reviewable in
the release PR — which is where the notes are actually read.

**What this tool is protecting.** CHANGELOG.md is over a thousand lines of released history. This
tool inserts one section at the top of it, and the failure mode to design against is not crashing:
it is silently destroying the history below and leaving a file that still looks like a changelog.
`scripts/cut-release.sh` used to run `git cliff -o CHANGELOG.md`, which regenerated the *whole*
file and did exactly that — the v0.0.x/v0.2.x sections still look nothing like the v0.3.x ones
because of it. So every guard below is a hard error, and the preservation check runs twice: once on
the text this process built, and again on the bytes that came back off the disk, because encoding
and newline translation happen at write time and would not be caught by checking our own string.

Guards:

  G1  the generated section has at least one bullet     — a range of only ci/chore/build/test
                                                          commits yields a bare heading
  G2  its heading is exactly `## [X.Y.Z] - <today>`     — catches --tag being ignored, and a date
                                                          scripts/tag-release.sh would later reject
  G3  no `## [X.Y.Z]` section already exists            — a second run fails instead of duplicating
  G4  the file has a `## [` anchor to splice above      — never write into a shape we do not know
  G5  everything from that anchor down is byte-identical
  G6  no `## [Unreleased]` heading is reintroduced      — that heading is the affordance that made
                                                          every pair of open PRs conflict

`--dry-run` prints the section and writes nothing.
"""

from __future__ import annotations

import argparse
import datetime
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

VERSION_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")

# The section anchor. Matches a released heading (`## [0.3.13] - 2026-07-28`) and, deliberately,
# `## [Unreleased]` too -- if one is ever hand-restored, splicing above it is still correct.
ANCHOR_RE = re.compile(r"^## \[", re.M)

CMAKE_PROJECT_RE = re.compile(r"^(project\(fighters-legacy VERSION )\d+\.\d+\.\d+", re.M)

# `striptags` and `protect_breaking_commits`, both used by cliff.toml, need a 2.x git-cliff.
MIN_GIT_CLIFF = (2, 0, 0)


class GenError(Exception):
    """A guard tripped. The message is written for whoever is cutting the release."""


# ---- file I/O ------------------------------------------------------------------------------------


def read_file(path: Path) -> str:
    """Read with the newlines the file actually has, so a CRLF checkout survives a round trip.

    `open(..., newline="")` rather than `Path.read_text(newline=...)`: that keyword only exists in
    Python 3.13, and `ubuntu-latest` runs 3.12.3. It went green on a Fedora box (3.14) and failed in
    CI with a bare `TypeError`. `Path.write_text` grew the keyword in 3.10 and would have worked --
    which is the trap, since read and write look symmetric and are not.
    """
    with open(path, encoding="utf-8", newline="") as handle:
        return handle.read()


def write_file(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


# ---- pure text operations (unit-tested; no git, no filesystem) ----------------------------------


def parse_version(tag: str) -> str:
    """`v1.2.3` -> `1.2.3`. The `v` prefix is the tag; the changelog heading has no prefix."""
    match = VERSION_RE.match(tag)
    if not match:
        raise GenError(f"version must be in vMAJOR.MINOR.PATCH format, got '{tag}'")
    return tag[1:]


def detect_newline(text: str) -> str:
    """The file's own line ending, so a CRLF checkout is not silently rewritten to LF."""
    return "\r\n" if "\r\n" in text else "\n"


def normalize_newlines(text: str, newline: str) -> str:
    return text.replace("\r\n", "\n").replace("\n", newline) if newline != "\n" else text.replace("\r\n", "\n")


def release_date() -> str:
    """Today in UTC -- the same clock git-cliff stamps the section with.

    Not local time. cliff.toml renders `{{ timestamp | date(...) }}`, which git-cliff evaluates in
    UTC, so a local `date.today()` disagrees with it for however many hours the local offset is --
    every evening west of Greenwich, every morning east of it. During that window G2 rejected a
    section git-cliff had just produced and the release simply could not be cut. UTC also makes the
    date independent of who runs the script and from where, which matters because the changelog
    date, the tag date and the release body all have to agree.
    """
    return datetime.datetime.now(datetime.timezone.utc).date().isoformat()


def check_generated(section: str, semver: str, today: str) -> None:
    """G1, G2, G6 -- validate git-cliff's output before it is allowed near the file."""
    lines = section.replace("\r\n", "\n").split("\n")

    if any(line.strip() == "## [Unreleased]" for line in lines):
        raise GenError(
            "git-cliff emitted an [Unreleased] heading, so --tag was not honoured. That heading is\n"
            "  the one #1123 removed -- it is what made every pair of open PRs conflict."
        )

    heading = next((line.strip() for line in lines if line.strip()), "")
    expected = f"## [{semver}] - {today}"
    if heading != expected:
        raise GenError(
            f"generated heading is '{heading}', expected '{expected}'.\n"
            "  scripts/tag-release.sh refuses to tag when the section date is not the tag date,\n"
            "  so a wrong date here becomes a blocked release later."
        )

    if not any(line.startswith("- ") for line in lines):
        raise GenError(
            f"generated section for {semver} has no entries.\n"
            "  Every commit in the range is ci/chore/build/test/style, which cliff.toml skips.\n"
            "  A release with nothing to tell a player about needs a deliberate decision, not a\n"
            "  bare heading -- write the section by hand, or reconsider cutting the release."
        )


def splice_section(original: str, section: str, semver: str) -> str:
    """Insert `section` immediately above the newest existing section. G3 and G4."""
    if re.search(rf"^## \[{re.escape(semver)}\]", original, re.M):
        raise GenError(
            f"CHANGELOG.md already has a [{semver}] section.\n"
            "  Cutting the same version twice would duplicate it. Drop the existing section or\n"
            "  pick the next version."
        )

    anchor = ANCHOR_RE.search(original)
    if not anchor:
        raise GenError(
            "CHANGELOG.md has no '## [' section heading to splice above.\n"
            "  Refusing to guess where the released history starts."
        )

    newline = detect_newline(original)
    body = normalize_newlines(section, newline).rstrip("\r\n") + newline * 2
    return original[: anchor.start()] + body + original[anchor.start() :]


def assert_preserved(original: str, updated: str, semver: str) -> None:
    """G5 -- everything from the old anchor down is byte-identical, re-derived independently.

    Deliberately does not trust how `splice_section` built the string: it re-finds the anchors in
    both texts and compares the slices. Run again on the bytes read back from disk, where it also
    covers encoding and newline translation at write time.
    """
    old_anchor = ANCHOR_RE.search(original)
    if not old_anchor:
        raise GenError("CHANGELOG.md has no '## [' section heading")

    anchors = list(ANCHOR_RE.finditer(updated))
    if len(anchors) < 2:
        raise GenError("the updated CHANGELOG.md has no section below the one just generated")
    if not updated.startswith(f"## [{semver}]", anchors[0].start()):
        raise GenError(f"the top section of the updated CHANGELOG.md is not [{semver}]")

    if updated[: anchors[0].start()] != original[: old_anchor.start()]:
        raise GenError("the CHANGELOG.md header changed -- refusing to write")
    if updated[anchors[1].start() :] != original[old_anchor.start() :]:
        raise GenError(
            "the released sections of CHANGELOG.md are not byte-identical -- refusing to write.\n"
            "  Generation applies forward only; nothing already released may move."
        )


def bump_cmake_version(text: str, semver: str) -> str:
    """Rewrite the root project() version. Python rather than `sed -i`, which is not portable."""
    matches = CMAKE_PROJECT_RE.findall(text)
    if len(matches) != 1:
        raise GenError(
            f"expected exactly one 'project(fighters-legacy VERSION ...)' line, found {len(matches)}"
        )
    return CMAKE_PROJECT_RE.sub(rf"\g<1>{semver}", text, count=1)


# ---- git-cliff ----------------------------------------------------------------------------------


def resolve_git_cliff() -> list[str]:
    """The git-cliff invocation to use, or a GenError naming how to install it."""
    if shutil.which("git-cliff"):
        argv = ["git-cliff"]
    elif shutil.which("git"):
        argv = ["git", "cliff"]
    else:
        argv = []

    if argv:
        try:
            out = subprocess.run(
                [*argv, "--version"], capture_output=True, text=True, check=True
            ).stdout
        except (subprocess.CalledProcessError, OSError):
            argv = []

    if not argv:
        raise GenError(
            "git-cliff is not installed. CHANGELOG.md is generated from commit subjects (#1123),\n"
            "  so cutting a release needs it:\n"
            "      Fedora:  sudo dnf install git-cliff\n"
            "      macOS:   brew install git-cliff\n"
            "      any:     cargo install git-cliff"
        )

    match = re.search(r"(\d+)\.(\d+)\.(\d+)", out)
    if not match or tuple(int(p) for p in match.groups()) < MIN_GIT_CLIFF:
        found = match.group(0) if match else out.strip()
        want = ".".join(str(p) for p in MIN_GIT_CLIFF)
        raise GenError(
            f"git-cliff {found} is too old; cliff.toml needs {want} or newer for `striptags` and\n"
            "  `protect_breaking_commits`. An older one would silently drop breaking changes and\n"
            "  emit the section-ordering markers into the file."
        )
    return argv


def run_git_cliff(argv: list[str], tag: str, repo_root: Path) -> str:
    """The unreleased range, rendered as the [X.Y.Z] section. Same config release.yml uses."""
    result = subprocess.run(
        [*argv, "--unreleased", "--tag", tag, "--strip", "header"],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise GenError(f"git-cliff failed (exit {result.returncode}):\n{result.stderr.strip()}")
    return result.stdout


# ---- driver --------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the next CHANGELOG.md section from conventional-commit subjects.",
        epilog="Run by scripts/cut-release.sh. See docs/developer/project-management.md.",
    )
    parser.add_argument("version", help="release tag, vMAJOR.MINOR.PATCH")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the generated section and write nothing",
    )
    parser.add_argument(
        "--repo-root", type=Path, default=REPO_ROOT, help=argparse.SUPPRESS
    )
    args = parser.parse_args(argv)

    try:
        semver = parse_version(args.version)
        today = release_date()

        changelog = args.repo_root / "CHANGELOG.md"
        cmakelists = args.repo_root / "CMakeLists.txt"

        section = run_git_cliff(resolve_git_cliff(), args.version, args.repo_root)
        check_generated(section, semver, today)

        if args.dry_run:
            print(section.rstrip("\r\n"))
            return 0

        original = read_file(changelog)
        updated = splice_section(original, section, semver)
        assert_preserved(original, updated, semver)

        write_file(changelog, updated)
        # Again, on what is actually on disk: the write itself is where encoding and newline
        # translation would corrupt the history we just checked in memory.
        assert_preserved(original, read_file(changelog), semver)

        write_file(cmakelists, bump_cmake_version(read_file(cmakelists), semver))

        entries = sum(1 for line in section.splitlines() if line.startswith("- "))
        print(f"CHANGELOG.md: wrote [{semver}] - {today} ({entries} entries)")
        print(f"CMakeLists.txt: project(fighters-legacy VERSION {semver})")
        return 0
    except GenError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
