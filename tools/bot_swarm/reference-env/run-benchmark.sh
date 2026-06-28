#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-benchmark.sh — runs INSIDE the constrained reference environment (container or VM).
# Builds fl-server + bot_swarm in Release and runs the load sweep. The repo is at $SRC.
#
# IMPORTANT: this builds Release (optimized). The ad-hoc dev-box numbers in #505 were Debug
# (-O0) builds and are pessimistic — always characterise on a Release build.
#
# Env knobs: SRC (default /src), BUILD (default /tmp/fl-ref-build), CLIENTS, DURATION, PATTERNS.
set -euo pipefail

SRC="${SRC:-/src}"
BUILD="${BUILD:-/tmp/fl-ref-build}"
CLIENTS="${CLIENTS:-64 128 256}"
DURATION="${DURATION:-30}"
PATTERNS="${PATTERNS:-idle weave}"

# `free` reads /proc/meminfo, which is NOT cgroup-aware (shows host RAM in a container). Report
# the cgroup v2 memory cap when present (container), else free (VM, where the cap IS the VM size).
_memmax="$(cat /sys/fs/cgroup/memory.max 2>/dev/null || echo max)"
if [[ "$_memmax" =~ ^[0-9]+$ ]]; then
    _mem="$((_memmax / 1024 / 1024 / 1024)) GiB (cgroup cap)"
else
    _mem="$(free -h 2>/dev/null | awk '/Mem:/{print $2}') RAM"
fi
echo "=== reference env: $(nproc) CPUs, ${_mem} ==="
# Headless server build: force Vulkan "not found" so the GPU renderer target (which needs
# glslangValidator) is skipped — fl-server + bot_swarm don't need it. Keeps the build deterministic
# even though SDL3's `dnf builddep` transitively pulls in the Vulkan loader.
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON >/dev/null
cmake --build "$BUILD" --target fl-server bot_swarm

# Each synthetic client is a UDP socket — raise the open-file limit toward the hard cap.
ulimit -n "$(ulimit -Hn 2>/dev/null || echo 4096)" 2>/dev/null || true

for p in $PATTERNS; do
    for n in $CLIENTS; do
        echo "############### ${n} clients, pattern=${p} ###############"
        # `|| true`: past the knee, run_loadtest.sh exits nonzero (clients dropped) — that's the
        # point of the sweep, so don't let `set -e`/pipefail abort the remaining runs.
        bash "$SRC/tools/bot_swarm/run_loadtest.sh" "$BUILD" "$n" "$DURATION" "$p" \
            | grep -E "clients:|tick-Hz|dn KB/s|RTT |loop dt|aggregate" || true
    done
done

echo "Reports written to: $SRC/tools/bot_swarm/results/"
