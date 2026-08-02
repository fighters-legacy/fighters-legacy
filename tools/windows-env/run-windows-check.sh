#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-windows-check.sh — host-side entry point for the Windows validation VM (#1114).
#
# Ships the current branch into the guest as a git bundle and runs the check tiers there, so an
# MSVC-only failure (C4456 shadowed local, C4244 narrowing, a missing <string> include) surfaces
# before the push rather than after it.
#
# OPTIONAL BY DESIGN. Nothing requires this script and no git hook calls it — run it when a branch
# looks MSVC-risky, skip it otherwise. Hosted CI stays the authority.
#
#   tools/windows-env/run-windows-check.sh                    # ci,smoke,runtime
#   FL_WINENV_TIERS=ci tools/windows-env/run-windows-check.sh  # just the build + ctest leg
#
# Env knobs: FL_WINENV_TIERS (default ci,smoke,runtime), FL_WINENV_GPU (1 = render on the
# passed-through GPU), FL_WINENV_RUNTIME_VIA_TASK (1 = scheduled-task fallback for the runtime tier).
#
# NOTE: it validates COMMITTED state. The bundle is built from commits, so anything sitting in the
# working tree is not what the guest compiles; the script says so before it starts.
set -euo pipefail

# ---- preflight ----
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$HERE" rev-parse --show-toplevel)"
TIERS="${FL_WINENV_TIERS:-ci,smoke,runtime}"

if ! command -v vagrant >/dev/null 2>&1; then
    echo "vagrant not found — see tools/windows-env/README.md for host prerequisites" >&2
    exit 1
fi

if [[ -n "$(git -C "$REPO" status --porcelain)" ]]; then
    echo "WARNING: the working tree is dirty. Only COMMITTED work reaches the guest:"
    git -C "$REPO" status --short | sed 's/^/    /'
    echo
fi

cd "$HERE"
if ! vagrant status --machine-readable 2>/dev/null | grep -q ",state,running"; then
    echo "=== bringing the VM up (first run also provisions: expect an hour or more) ==="
    vagrant up --provider=libvirt
fi

# ---- bundle the branch ----
# A bundle of merge-base(origin/main, HEAD)..HEAD is small and lets the guest's persistent clone
# fast-forward without network access. When HEAD is already an ancestor of what origin has, there
# is nothing to bundle and the guest just fetches.
SHA="$(git -C "$REPO" rev-parse HEAD)"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

echo "=== staging $SHA ==="
if BASE="$(git -C "$REPO" merge-base origin/main HEAD 2>/dev/null)" && [[ "$BASE" != "$SHA" ]]; then
    git -C "$REPO" bundle create "$SCRATCH/head.bundle" "$BASE..HEAD" 2>&1 | sed 's/^/    /'
    vagrant upload "$SCRATCH/head.bundle" 'C:\fl\incoming\head.bundle'
else
    echo "    HEAD is already on origin — the guest will fetch instead of taking a bundle"
    vagrant winrm -s powershell -c "Remove-Item C:\fl\incoming\head.bundle -ErrorAction SilentlyContinue" >/dev/null 2>&1 || true
fi

# Upload the driver from the host working tree rather than relying on the guest checkout, so edits
# to the harness itself take effect on the very run that makes them.
vagrant upload run-checks.ps1 'C:\fl\windows-env\run-checks.ps1'

# ---- run the checks ----
echo "=== running tiers [$TIERS] in the guest ==="
status=0
vagrant winrm -s powershell -c "\$env:FL_WINENV_TIERS='$TIERS'; \
\$env:FL_WINENV_GPU='${FL_WINENV_GPU:-}'; \
\$env:FL_WINENV_RUNTIME_VIA_TASK='${FL_WINENV_RUNTIME_VIA_TASK:-}'; \
pwsh -NoProfile -ExecutionPolicy Bypass -File C:\\fl\\windows-env\\run-checks.ps1 -Sha $SHA" \
    || status=$?

# ---- collect artifacts ----
# Best-effort: a failed run is exactly when the screenshot is worth looking at, so this happens
# regardless of status and never changes it.
RESULTS="$HERE/results"
mkdir -p "$RESULTS"
if vagrant winrm -s powershell -c "if (Test-Path C:\fl\out\runtime-smoke.png) { \
[Convert]::ToBase64String([IO.File]::ReadAllBytes('C:\fl\out\runtime-smoke.png')) }" \
        2>/dev/null | tr -d '\r\n ' | base64 -d > "$RESULTS/runtime-smoke.png" 2>/dev/null \
        && [[ -s "$RESULTS/runtime-smoke.png" ]]; then
    echo "screenshot: $RESULTS/runtime-smoke.png"
else
    rm -f "$RESULTS/runtime-smoke.png"
fi

exit "$status"
