#!/usr/bin/env bash
# Creates a release branch, rolls the CHANGELOG heading, bumps the CMake version, commits, pushes.
# After the PR merges, run: ./scripts/tag-release.sh vX.Y.Z
#
# This script used to run `git cliff --tag "$VERSION" -o CHANGELOG.md`, which REGENERATED the whole
# file from conventional commits. CHANGELOG.md is hand-curated — every entry is written per issue
# with its own rationale — so regenerating it destroyed all of that and replaced it with one terse
# PR-keyed line per squash commit. That is why the v0.0.x/v0.2.x sections look nothing like the
# v0.3.x ones: the script was used, then abandoned, and the docs never caught up.
#
# The roll below is therefore purely mechanical: rename the [Unreleased] heading and open a fresh
# empty one above it. Nothing is generated, nothing is overwritten.
#
# The full procedure — including the steps AFTER the tag, which are the ones that get skipped —
# is docs/developer/project-management.md, "Cutting a release".
set -euo pipefail

VERSION="${1:?Usage: $0 vMAJOR.MINOR.PATCH}"

if [[ ! "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: version must be in vMAJOR.MINOR.PATCH format" >&2
    exit 1
fi

BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [[ "$BRANCH" != "main" ]]; then
    echo "Error: must be on main (currently on '$BRANCH')" >&2
    exit 1
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "Error: working tree is not clean" >&2
    exit 1
fi

git pull origin main

SEMVER="${VERSION#v}"
TODAY=$(date +%F)

RELEASE_BRANCH="release/$VERSION"
git checkout -b "$RELEASE_BRANCH"

# Roll [Unreleased] -> [X.Y.Z] - <today>, leaving a fresh empty [Unreleased] above it.
# The date must end up matching the TAG date; tag-release.sh refuses to tag if they disagree.
python3 - "$SEMVER" "$TODAY" <<'PY'
import sys, re
semver, today = sys.argv[1], sys.argv[2]
path = "CHANGELOG.md"
text = open(path, encoding="utf-8").read()

if re.search(rf"^## \[{re.escape(semver)}\]", text, re.M):
    sys.exit(f"Error: CHANGELOG.md already has a [{semver}] section")
if not re.search(r"^## \[Unreleased\]", text, re.M):
    sys.exit("Error: CHANGELOG.md has no [Unreleased] section to roll")

text = re.sub(r"^## \[Unreleased\][^\n]*$",
              f"## [Unreleased]\n\n## [{semver}] - {today}",
              text, count=1, flags=re.M)
open(path, "w", encoding="utf-8").write(text)
print(f"rolled [Unreleased] -> [{semver}] - {today}")
PY

sed -i "s/^project(fighters-legacy VERSION [0-9]*\.[0-9]*\.[0-9]*/project(fighters-legacy VERSION $SEMVER/" CMakeLists.txt

git add CHANGELOG.md CMakeLists.txt
git commit -s -m "chore(release): $VERSION"

git push -u origin "$RELEASE_BRANCH"

cat <<EOF

Branch '$RELEASE_BRANCH' pushed.

BEFORE OPENING THE PR, edit the new [$SEMVER] section:

  * CONDENSE it to one line per change with an issue ref — '- **scope**: Headline (#NNN)'.
    The long rationale accumulated under [Unreleased] is deliberately dropped; it lives on in
    the PR bodies and commit messages. A changelog is an index, not an archive.
  * Merge any duplicate '### Changed' blocks that accumulated.
  * Check the scope against the range:  git log --oneline <prev-tag>..HEAD
    Every released issue needs an entry. Nothing from a PRIOR release may be left floating
    under [Unreleased] for the next release to sweep up.

Open a PR: https://github.com/fighters-legacy/fighters-legacy/compare/$RELEASE_BRANCH
After merging, run: ./scripts/tag-release.sh $VERSION

The release is NOT done when the tag is pushed. See docs/developer/project-management.md,
"Cutting a release" — the hand-authored release notes are step 6, and they are the step
that has been skipped most often.
EOF
