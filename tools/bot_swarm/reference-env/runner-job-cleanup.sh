#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# runner-job-cleanup.sh — GitHub Actions job hook for the self-hosted reference runner (issue #569).
# Wired to ACTIONS_RUNNER_HOOK_JOB_STARTED and ACTIONS_RUNNER_HOOK_JOB_COMPLETED (in the runner's
# .env by setup-self-hosted-runner.sh). It is the workspace-hygiene compensating control for a
# NON-EPHEMERAL runner: reclaim the multi-GB Release build tree and reset any state a previous job
# left behind, so each strict-tier run starts from a clean slate.
#
# Must never fail a job — every action is best-effort (`|| true`).
set -uo pipefail

# The job hooks run with the runner's default environment. Prefer the checkout path; fall back to the
# runner work root derived from this script's location (<runner_home>/runner-job-cleanup.sh).
WS="${GITHUB_WORKSPACE:-${RUNNER_WORKSPACE:-}}"
if [[ -z "$WS" ]]; then
    WS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_work"
fi

echo "[runner-cleanup] tidying '$WS'"
if [[ -d "$WS/.git" ]]; then
    # Drop ignored artifacts too (-x): the Release build/ tree is the big reclaim.
    git -C "$WS" clean -xffd || true
    git -C "$WS" reset --hard || true
elif [[ -d "$WS" ]]; then
    find "$WS" -mindepth 1 -maxdepth 1 -exec rm -rf {} + || true
fi
exit 0
