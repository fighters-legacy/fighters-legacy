#!/usr/bin/env bash
# Creates a release branch, generates the CHANGELOG section, bumps the CMake version, commits, pushes.
# After the PR merges, run: ./scripts/tag-release.sh vX.Y.Z
#
# CHANGELOG.md is GENERATED from conventional-commit subjects by git-cliff (#1123) — no PR writes
# into it, so there is nothing hand-maintained left to roll. scripts/gen_changelog.py does the
# generation and carries the guards; read its docstring before changing anything here.
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
PREV_TAG=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null || echo "")

RELEASE_BRANCH="release/$VERSION"
git checkout -b "$RELEASE_BRANCH"

# Writes the [X.Y.Z] section above the newest existing one and bumps CMakeLists.txt. Refuses rather
# than guesses: an empty section, a wrong date, a duplicate version, or any change to the released
# sections below is a hard error. The date it stamps must be the day the tag is created —
# tag-release.sh refuses to tag if they disagree.
python3 "$(dirname "$0")/gen_changelog.py" "$VERSION"

git add CHANGELOG.md CMakeLists.txt
git commit -s -m "chore(release): $VERSION"

git push -u origin "$RELEASE_BRANCH"

cat <<EOF

Branch '$RELEASE_BRANCH' pushed.

BEFORE OPENING THE PR, read the generated [$SEMVER] section as a PLAYER would:

  * Each bullet is a commit subject, published verbatim. This is the LAST point at which a
    subject that reads as a note-to-self can be fixed — after the release PR merges it is
    history. Reword one by editing the section directly, and say so in the PR.
  * Check the scope against the range:  git log --oneline ${PREV_TAG:-<prev-tag>}..HEAD
    Every user-facing change should be there. cliff.toml deliberately skips ci/chore/build/
    test/style, so a commit missing from the section is either correctly skipped or was typed
    with the wrong conventional-commit type.

Open a PR: https://github.com/fighters-legacy/fighters-legacy/compare/$RELEASE_BRANCH
After merging, run: ./scripts/tag-release.sh $VERSION

The release is NOT done when the tag is pushed. See docs/developer/project-management.md,
"Cutting a release" — the hand-authored release notes are step 6, and they are the step
that has been skipped most often.
EOF
