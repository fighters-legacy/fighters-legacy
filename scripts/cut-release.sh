#!/usr/bin/env bash
# Creates a release branch, generates CHANGELOG.md, commits, and pushes.
# After the PR merges, run: ./scripts/tag-release.sh vX.Y.Z
#
# Requires git-cliff: cargo install git-cliff | dnf install git-cliff | brew install git-cliff
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

RELEASE_BRANCH="release/$VERSION"
git checkout -b "$RELEASE_BRANCH"

git cliff --tag "$VERSION" -o CHANGELOG.md

git add CHANGELOG.md
git commit -s -m "chore(release): prepare $VERSION"

git push -u origin "$RELEASE_BRANCH"

echo ""
echo "Branch '$RELEASE_BRANCH' pushed."
echo "Open a PR: https://github.com/jomkz/fighters-legacy/compare/$RELEASE_BRANCH"
echo "After merging, run: ./scripts/tag-release.sh $VERSION"
