#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-container.sh — Linux / macOS host wrapper. Builds the reference image and runs the load
# sweep in a container constrained to the reference profile (8 cores / 16 GB). Uses podman if
# present, else docker.
#
# Knobs (env): CPUS (default 8), CPUSET (default 0-7), MEM (default 16g), CLIENTS, DURATION,
#              PATTERNS, ENGINE (podman|docker), IMAGE.
#
# Topology note: CPUSET pins to *specific* logical CPUs. On a hybrid CPU (e.g. Intel P/E cores)
# pick 8 performance-core threads (often 0-7) so the guest sees uniform cores like a cloud vCPU.
# On macOS, --cpuset-cpus is honoured inside the podman/Docker Linux VM, which must itself be
# allocated >= 8 CPUs / 16 GB (see README).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

ENGINE="${ENGINE:-$(command -v podman || command -v docker || true)}"
[ -n "$ENGINE" ] || { echo "ERROR: need podman or docker on PATH"; exit 1; }

CPUS="${CPUS:-8}"
CPUSET="${CPUSET:-0-7}"
MEM="${MEM:-16g}"
IMAGE="${IMAGE:-fl-bot-swarm-refenv}"

echo "=== building image ($ENGINE) ==="
"$ENGINE" build -t "$IMAGE" -f "$HERE/Containerfile" "$HERE"

# CPU pinning needs the cpuset cgroup controller. Rootless podman only gets the controllers
# systemd delegates to the user slice — cpuset is usually NOT delegated, so probe for it and
# fall back to a --cpus quota. Pinning (nproc == 8 in the guest) is more faithful; the quota
# fallback throttles CPU *time* but the guest still sees all host cores. For true core-count
# fidelity: enable delegation (see README), run rootful (sudo), or use the Vagrant VM.
CPUSET_ARG=()
if [ -n "$CPUSET" ]; then
    if "$ENGINE" run --rm --cpuset-cpus "${CPUSET%%[,-]*}" "$IMAGE" true >/dev/null 2>&1; then
        CPUSET_ARG=(--cpuset-cpus "$CPUSET")
    else
        echo "[WARN] cpuset cgroup unavailable (rootless without delegation?) — using --cpus quota."
        echo "[WARN] guest will see all host cores; see reference-env/README.md for true 8-core fidelity."
        CPUSET=""
    fi
fi

echo "=== running sweep: ${CPUS} CPUs (cpuset=${CPUSET:-quota-only}) / ${MEM} ==="
"$ENGINE" run --rm -t \
    --cpus "$CPUS" "${CPUSET_ARG[@]}" --memory "$MEM" --memory-swap "$MEM" \
    --security-opt label=disable \
    -e CLIENTS="${CLIENTS:-64 128 256}" \
    -e DURATION="${DURATION:-30}" \
    -e PATTERNS="${PATTERNS:-idle weave}" \
    -v "$REPO":/src \
    "$IMAGE" bash /src/tools/bot_swarm/reference-env/run-benchmark.sh

echo "Reports: $REPO/tools/bot_swarm/results/"
