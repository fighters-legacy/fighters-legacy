#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-benchmark.sh — runs INSIDE the constrained reference environment (container or VM).
# Builds fl-server + bot_swarm in Release and runs the load sweep. The repo is at $SRC.
#
# IMPORTANT: this builds Release (optimized). The ad-hoc dev-box numbers in #505 were Debug
# (-O0) builds and are pessimistic — always characterise on a Release build.
#
# Env knobs: SRC (default /src), BUILD (default /tmp/fl-ref-build), CLIENTS, DURATION, PATTERNS,
# ENTITY_COUNTS, SIM_WORKERS, GNS.
set -euo pipefail

SRC="${SRC:-/src}"
BUILD="${BUILD:-/tmp/fl-ref-build}"
CLIENTS="${CLIENTS:-64 128 256}"
DURATION="${DURATION:-30}"
PATTERNS="${PATTERNS:-idle weave}"
# Entity-pool + SpatialIndex scaling sweep (#573). ENTITY_COUNTS pre-spawns N server-side AI entities
# per run (FL_TEST_SPAWN_AI); SIM_WORKERS sweeps the data-parallel sim worker count (#511,
# FL_SIM_WORKER_THREADS). Defaults reproduce the pre-#573 behaviour: no extra entities, server-default
# worker count ("" = let the server auto-pick). Example matrix:
#   ENTITY_COUNTS="0 2000 5000" SIM_WORKERS="1 4 8" PATTERNS=weave CLIENTS=64 run-benchmark.sh
ENTITY_COUNTS="${ENTITY_COUNTS:-0}"
SIM_WORKERS="${SIM_WORKERS:-}"
# GNS=1 (default) builds the GameNetworkingSockets transport, which is what the `reference` and
# `soak` profiles measure. Set GNS=0 for a deliberate enet6-only sweep. There is no third state:
# dependencies.cmake force-disables GNS when its dependencies are missing, so without the assertion
# below a GNS profile would build enet6 and report numbers for a transport nobody asked about
# (#1136). FL_ALLOW_SHARED_PROTOBUF is set because this binary never leaves the machine that built
# it and Fedora ships no static libprotobuf — see the option's comment in CMakeLists.txt.
GNS="${GNS:-1}"

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
# glslangValidator) is skipped — fl-server + bot_swarm don't need it (they link no SDL3/Vulkan).
gns_flags=()
if [[ "$GNS" != "0" ]]; then
    gns_flags=(-DFL_ENABLE_GNS=ON -DFL_ALLOW_SHARED_PROTOBUF=ON)
fi
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON "${gns_flags[@]}" >/dev/null

# Assert the transport we asked for is the transport we are about to measure — the same check
# scale-gate.yml runs (#1136). A force-disabled GNS is a silent substitution: the build succeeds,
# every run completes, and the report describes enet6 under a gns profile's name. Fail at build
# time, where the cause is still on screen, rather than at leg time.
if [[ "$GNS" != "0" ]] && ! grep -qx 'FL_ENABLE_GNS:BOOL=ON' "$BUILD/CMakeCache.txt"; then
    echo "ERROR: FL_ENABLE_GNS was force-disabled during configure — this environment cannot build" >&2
    echo "       the GNS transport, so its gns profiles would silently measure enet6. Install the" >&2
    echo "       OpenSSL + protobuf packages (see the Containerfile / vm-provision.sh), or run with" >&2
    echo "       GNS=0 for a deliberate enet6-only sweep." >&2
    exit 1
fi
cmake --build "$BUILD" --target fl-server bot_swarm

# Each synthetic client is a UDP socket — raise the open-file limit toward the hard cap.
ulimit -n "$(ulimit -Hn 2>/dev/null || echo 4096)" 2>/dev/null || true

for p in $PATTERNS; do
    for n in $CLIENTS; do
        for e in $ENTITY_COUNTS; do
            # SIM_WORKERS may be empty (server default) → iterate a single sentinel pass.
            for w in ${SIM_WORKERS:-__default__}; do
                if [[ "$w" == "__default__" ]]; then
                    unset FL_SIM_WORKER_THREADS
                    wlabel="default"
                else
                    export FL_SIM_WORKER_THREADS="$w"
                    wlabel="$w"
                fi
                export FL_TEST_SPAWN_AI="$e"
                echo "############### ${n} clients, pattern=${p}, ai_entities=${e}, sim_workers=${wlabel} ###############"
                # `|| true`: past the knee, run_loadtest.sh exits nonzero (clients dropped) — that's the
                # point of the sweep, so don't let `set -e`/pipefail abort the remaining runs.
                bash "$SRC/tools/bot_swarm/run_loadtest.sh" "$BUILD" "$n" "$DURATION" "$p" \
                    | grep -E "clients:|tick-Hz|dn KB/s|RTT |loop dt|aggregate|server tick" || true
            done
        done
    done
done
unset FL_TEST_SPAWN_AI FL_SIM_WORKER_THREADS 2>/dev/null || true

echo "Reports written to: $SRC/tools/bot_swarm/results/"
