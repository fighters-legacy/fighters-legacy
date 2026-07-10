#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# setup-self-hosted-runner.sh — register a GitHub Actions self-hosted runner for the scale-gate
# strict tier (issue #569). Runs INSIDE the 8-vCPU/16 GB reference VM (the Vagrantfile guest) or on
# any bare Fedora box; it is the reproducible, version-controlled form of the manual registration
# runbook (see README.md → "Self-hosted reference runner"). Idempotent: safe to re-run.
#
# The runner is NON-EPHEMERAL (a warm box gives comparable benchmark timing) and is the enforcement
# point for the `≤ 16.6 ms p99` strict tick gate that hosted runners can only advise on.
#
# SECURITY (public repo + self-hosted): the VM is a disposable sandbox, and the scale-gate workflow
# only reaches this runner via `workflow_dispatch` (write-access-gated) behind a repo guard — no
# fork/`pull_request` code path executes here. This script hardens further: a dedicated unprivileged
# `flrunner` user, a systemd hardening drop-in, and a per-job workspace-cleanup hook.
#
# Usage (as root, e.g. via `sudo`):
#     FL_RUNNER_TOKEN=<registration-token> bash setup-self-hosted-runner.sh
#
#   FL_RUNNER_TOKEN     required. Short-lived registration token from the repo:
#                       Settings → Actions → Runners → New self-hosted runner.
#   FL_RUNNER_VERSION   optional. actions/runner version (default: latest release via API,
#                       falling back to the pinned FALLBACK_VERSION below).
#   FL_RUNNER_SHA256    optional. If set, the downloaded tarball is verified against it (the exact
#                       value is shown on the same "New self-hosted runner" page).
#   FL_RUNNER_URL       optional. Repo URL (default: the canonical fighters-legacy repo).
#   FL_RUNNER_NAME      optional. Runner name (default: fl-reference-vm).
set -euo pipefail

# ---- config ------------------------------------------------------------------------------------
RUNNER_USER="flrunner"
RUNNER_HOME="/opt/actions-runner"
RUNNER_URL="${FL_RUNNER_URL:-https://github.com/fighters-legacy/fighters-legacy}"
RUNNER_NAME="${FL_RUNNER_NAME:-fl-reference-vm}"
# Custom label only — the runner auto-applies self-hosted,linux,x64; together they satisfy the
# workflow's `runs-on: [self-hosted, linux, x64, fl-reference]`.
RUNNER_LABELS="fl-reference"
FALLBACK_VERSION="2.328.0" # used only if the GitHub API lookup fails; override with FL_RUNNER_VERSION

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$(id -u)" -ne 0 ]]; then
    echo "ERROR: run as root (needs to create a user + install a systemd service). Try: sudo $0" >&2
    exit 1
fi
if [[ -z "${FL_RUNNER_TOKEN:-}" ]]; then
    echo "ERROR: FL_RUNNER_TOKEN is required (repo Settings → Actions → Runners → New)." >&2
    exit 1
fi

# ---- toolchain (defensive; the Vagrant provisioner already ran vm-provision.sh) -----------------
# libicu is the actions/runner runtime dep; curl/tar fetch+unpack the runner; the build toolchain
# lets a job configure+build fl-server+bot_swarm. Reuses vm-provision.sh's list so the box matches
# the reference toolchain even when this script is run standalone on a bare box. No SDL3 build deps:
# fl-server (platform-stdfs) + bot_swarm (enet6) are headless and link no SDL3 (#711/#716; #718).
echo "=== ensuring toolchain (dnf) ==="
dnf -y install \
    libicu curl tar \
    gcc-c++ cmake ninja-build git findutils procps-ng

# ---- dedicated unprivileged runner user --------------------------------------------------------
if ! id "$RUNNER_USER" >/dev/null 2>&1; then
    echo "=== creating unprivileged user '$RUNNER_USER' ==="
    # System user, no login shell, home = $RUNNER_HOME (installs off the rsync'd /src so jobs
    # check out fresh and can never touch the host working tree).
    useradd --system --create-home --home-dir "$RUNNER_HOME" --shell /usr/sbin/nologin "$RUNNER_USER"
fi
mkdir -p "$RUNNER_HOME"
chown "$RUNNER_USER:$RUNNER_USER" "$RUNNER_HOME"

# ---- download the runner -----------------------------------------------------------------------
VERSION="${FL_RUNNER_VERSION:-}"
if [[ -z "$VERSION" ]]; then
    VERSION="$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest 2>/dev/null \
        | grep -oE '"tag_name": *"v[0-9.]+"' | grep -oE '[0-9.]+' | head -n1 || true)"
    VERSION="${VERSION:-$FALLBACK_VERSION}"
fi
TARBALL="actions-runner-linux-x64-${VERSION}.tar.gz"
if [[ ! -f "$RUNNER_HOME/config.sh" ]]; then
    echo "=== downloading actions/runner ${VERSION} ==="
    curl -fsSL -o "/tmp/${TARBALL}" \
        "https://github.com/actions/runner/releases/download/v${VERSION}/${TARBALL}"
    if [[ -n "${FL_RUNNER_SHA256:-}" ]]; then
        echo "${FL_RUNNER_SHA256}  /tmp/${TARBALL}" | sha256sum -c -
    fi
    tar -xzf "/tmp/${TARBALL}" -C "$RUNNER_HOME"
    rm -f "/tmp/${TARBALL}"
    chown -R "$RUNNER_USER:$RUNNER_USER" "$RUNNER_HOME"
fi

# ---- configure (must NOT run as root — the runner refuses) --------------------------------------
if [[ ! -f "$RUNNER_HOME/.runner" ]]; then
    echo "=== configuring runner '$RUNNER_NAME' (labels: self-hosted,linux,x64,$RUNNER_LABELS) ==="
    sudo -u "$RUNNER_USER" -- "$RUNNER_HOME/config.sh" \
        --url "$RUNNER_URL" \
        --token "$FL_RUNNER_TOKEN" \
        --labels "$RUNNER_LABELS" \
        --name "$RUNNER_NAME" \
        --unattended --replace
else
    echo "=== runner already configured ($RUNNER_HOME/.runner exists); skipping config ==="
fi

# ---- per-job workspace cleanup hook (non-ephemeral hygiene) -------------------------------------
install -m 0755 -o "$RUNNER_USER" -g "$RUNNER_USER" \
    "$SCRIPT_DIR/runner-job-cleanup.sh" "$RUNNER_HOME/runner-job-cleanup.sh"
# The runner reads .env on service start; wire the cleanup hook to both job phases.
ENV_FILE="$RUNNER_HOME/.env"
touch "$ENV_FILE"
for key in ACTIONS_RUNNER_HOOK_JOB_STARTED ACTIONS_RUNNER_HOOK_JOB_COMPLETED; do
    grep -q "^${key}=" "$ENV_FILE" 2>/dev/null \
        && sed -i "s|^${key}=.*|${key}=${RUNNER_HOME}/runner-job-cleanup.sh|" "$ENV_FILE" \
        || echo "${key}=${RUNNER_HOME}/runner-job-cleanup.sh" >>"$ENV_FILE"
done
chown "$RUNNER_USER:$RUNNER_USER" "$ENV_FILE"

# ---- install + harden + start the systemd service ----------------------------------------------
echo "=== installing systemd service (user: $RUNNER_USER) ==="
( cd "$RUNNER_HOME" && ./svc.sh install "$RUNNER_USER" )

# svc.sh names the unit actions.runner.<owner>-<repo>.<name>.service — discover it and drop in the
# hardening overrides committed alongside this script.
UNIT_FILE="$(ls /etc/systemd/system/actions.runner.*."${RUNNER_NAME}".service 2>/dev/null | head -n1 || true)"
if [[ -n "$UNIT_FILE" ]]; then
    DROPIN_DIR="${UNIT_FILE}.d"
    mkdir -p "$DROPIN_DIR"
    install -m 0644 "$SCRIPT_DIR/runner-hardening.conf" "$DROPIN_DIR/hardening.conf"
    systemctl daemon-reload
    echo "=== applied hardening drop-in: $DROPIN_DIR/hardening.conf ==="
else
    echo "[WARN] could not locate the runner unit to apply runner-hardening.conf; apply it manually." >&2
fi

( cd "$RUNNER_HOME" && ./svc.sh start )
echo
echo "=== done. Verify with: sudo $RUNNER_HOME/svc.sh status ==="
echo "The runner should now show as Idle in repo Settings → Actions → Runners (label: fl-reference)."
