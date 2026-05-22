#!/usr/bin/env bash
# Tags main with vX.Y.Z and pushes to trigger the release workflow.
# Run after the release PR (created by cut-release.sh) is merged.
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

git pull origin main
git tag -a "$VERSION" -m "$VERSION"
git push origin "$VERSION"

echo ""
echo "Tag $VERSION pushed. Release workflow:"
echo "  https://github.com/jomkz/fighters-legacy/actions/workflows/release.yml"
