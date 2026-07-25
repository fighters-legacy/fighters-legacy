#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# measure_macos.sh — LLM inference vs Vulkan renderer GPU contention, macOS / Metal leg (#782).
#
# Mirror of measure_linux.sh; see that script for the method. The differences are all in what the
# platform can tell you about memory:
#
#   * Apple Silicon has UNIFIED memory. There is no discrete VRAM pool and no per-process GPU
#     memory API, so the "headroom" number means something different here than on a discrete GPU:
#     the model and the renderer are competing for the same system RAM, and the ceiling is the
#     machine's RAM, not a card's.
#   * The renderer runs on MoltenVK, so VK_EXT_memory_budget (what FrameStats reports) describes a
#     Metal heap's RECOMMENDED WORKING SET, not a dedicated allocation. Read it as a trend across
#     the run's phases, not as an absolute.
#
# The Metal-side numbers therefore come from `ollama ps` (the model's own footprint) plus
# system-level memory pressure, both recorded into the sysinfo file for the report.
#
# Usage / options: identical to measure_linux.sh (run with --help).
#
# BEFORE RUNNING: pin the model in memory (`export OLLAMA_KEEP_ALIVE=-1`). An evicted model
# reloads INSIDE a burst and that load is measured as contention rather than as what it is.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MODEL="qwen2.5-coder:14b"
BASE_URL="${FL_AI_BASE_URL:-http://localhost:11434}"
WORKLOAD="intent"
MISSION="builtin:sandbox"
CONCURRENCY=1
BURSTS=5
BURST_SECONDS=20
IDLE_SECONDS=60
GAP_SECONDS=20
TAIL_SECONDS=30
REPEAT=1
LABEL=""
BUILD_DIR="$REPO_ROOT/build/release"

while [[ $# -gt 0 ]]; do
    case "$1" in
    --model) MODEL="$2"; shift ;;
    --base-url) BASE_URL="$2"; shift ;;
    --workload) WORKLOAD="$2"; shift ;;
    --mission) MISSION="$2"; shift ;;
    --concurrency) CONCURRENCY="$2"; shift ;;
    --bursts) BURSTS="$2"; shift ;;
    --burst-seconds) BURST_SECONDS="$2"; shift ;;
    --idle-seconds) IDLE_SECONDS="$2"; shift ;;
    --gap-seconds) GAP_SECONDS="$2"; shift ;;
    --tail-seconds) TAIL_SECONDS="$2"; shift ;;
    --repeat) REPEAT="$2"; shift ;;
    --label) LABEL="$2"; shift ;;
    -h | --help) sed -n '3,25p' "$0"; exit 0 ;;
    *) BUILD_DIR="$1" ;;
    esac
    shift
done

PORT="${FL_CONTENTION_PORT:-4796}"
GAME="$BUILD_DIR/game/fighters-legacy/fighters-legacy"
FLSERVER="$BUILD_DIR/server/fl-server/fl-server"
RESULTS="$SCRIPT_DIR/results"
STAMP="$(date -u +"%Y%m%dT%H%M%SZ")"
SUFFIX="macos${LABEL:+_$LABEL}_$STAMP"
mkdir -p "$RESULTS"

# ── Preflight ────────────────────────────────────────────────────────────────────────────────
# A .app bundle build puts the binary inside Contents/MacOS; accept either layout.
[[ -x "$GAME" ]] || GAME="$BUILD_DIR/game/fighters-legacy/fighters-legacy.app/Contents/MacOS/fighters-legacy"
[[ -x "$GAME" ]] || { echo "ERROR: game binary not found under $BUILD_DIR (build a Release tree first)"; exit 1; }
[[ -x "$FLSERVER" ]] || { echo "ERROR: fl-server not found at $FLSERVER"; exit 1; }
curl -fsS --max-time 5 "$BASE_URL/v1/models" >/dev/null 2>&1 ||
    curl -fsS --max-time 5 "$BASE_URL/api/tags" >/dev/null 2>&1 ||
    { echo "ERROR: no OpenAI-compatible endpoint answering at $BASE_URL"; exit 1; }

SCHEDULE_S="$(python3 "$SCRIPT_DIR/driver.py" --model "$MODEL" --print-schedule \
    --idle-seconds "$IDLE_SECONDS" --bursts "$BURSTS" --burst-seconds "$BURST_SECONDS" \
    --gap-seconds "$GAP_SECONDS" --tail-seconds "$TAIL_SECONDS")"
RUN_SECONDS="$(python3 -c "import sys; print(int(float(sys.argv[1])) + 150)" "$SCHEDULE_S")"

echo "── GPU contention (#782), macOS / Metal ──"
echo "  endpoint : $BASE_URL   model: $MODEL   workload: $WORKLOAD (concurrency $CONCURRENCY)"
echo "  scene    : $MISSION    schedule: ${SCHEDULE_S}s   game run: ${RUN_SECONDS}s   repeat: ${REPEAT}"
echo "  results  : $RESULTS/*_$SUFFIX*.*"

# ── System info + memory before anything is loaded ───────────────────────────────────────────
SYSINFO="$RESULTS/sysinfo_$SUFFIX.txt"
{
    echo "date_utc: $STAMP"
    echo "os: $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "chip: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    echo "cores: $(sysctl -n hw.ncpu)"
    echo "mem_total_bytes: $(sysctl -n hw.memsize)"
    echo "gpu: $(system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/ {print $2; exit}')"
    # Unified memory: there is no per-process VRAM query, so record system-wide pressure instead.
    echo "memory_pressure_before: $(memory_pressure 2>/dev/null | tail -1)"
    echo "ollama_ps_before: $(ollama ps 2>/dev/null | tail -n +2 | tr '\n' ';')"
} >"$SYSINFO"
echo "  sysinfo  : $SYSINFO"

# ── Boot the server ONCE; all repeats run against the same warm server + model ────────────────
WORKDIR="$(mktemp -d)"
SERVER_LOG="$WORKDIR/server.log"
GAME_PID=""
cleanup() {
    kill "${GAME_PID:-0}" 2>/dev/null || true
    kill "${SERVER_PID:-0}" 2>/dev/null || true
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

"$FLSERVER" "$PORT" 8 --bind 127.0.0.1 --mission "$MISSION" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
    grep -q "listening on" "$SERVER_LOG" 2>/dev/null && break
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "ERROR: fl-server exited early:"; tail -5 "$SERVER_LOG"; exit 1; }
    sleep 0.2
done
grep -q "listening on" "$SERVER_LOG" || { echo "ERROR: fl-server never came up:"; tail -5 "$SERVER_LOG"; exit 1; }

# ── One measurement pass (observer ghost, windowed — see measure_linux.sh for why) ────────────
REPORTS=()
run_once() {
    local run="$1" tag frames driver report
    tag="${SUFFIX}$([[ "$REPEAT" -gt 1 ]] && printf '_run%02d' "$run")"
    frames="$RESULTS/frames_$tag.json"
    driver="$RESULTS/driver_$tag.json"
    report="$RESULTS/macos${LABEL:+_$LABEL}_${STAMP}$([[ "$REPEAT" -gt 1 ]] && printf '_run%02d' "$run")"

    "$GAME" --connect "127.0.0.1:$PORT" --observer --auto \
        --frame-stats-json "$frames" --run-seconds "$RUN_SECONDS" >"$WORKDIR/game_$run.log" 2>&1 &
    GAME_PID=$!

    echo "  [run $run/$REPEAT] waiting for the client to reach Flight and start recording..."
    local i
    for i in $(seq 1 150); do
        [[ -s "$frames" ]] && break
        kill -0 "$GAME_PID" 2>/dev/null || { echo "ERROR: game exited early:"; tail -20 "$WORKDIR/game_$run.log"; exit 1; }
        sleep 1
    done
    [[ -s "$frames" ]] || { echo "ERROR: no frame stats after 150 s:"; tail -20 "$WORKDIR/game_$run.log"; exit 1; }

    python3 "$SCRIPT_DIR/driver.py" \
        --base-url "$BASE_URL" --model "$MODEL" --workload "$WORKLOAD" --concurrency "$CONCURRENCY" \
        --idle-seconds "$IDLE_SECONDS" --bursts "$BURSTS" --burst-seconds "$BURST_SECONDS" \
        --gap-seconds "$GAP_SECONDS" --tail-seconds "$TAIL_SECONDS" --out "$driver"

    # Unified memory has no per-process VRAM query — record system-wide pressure on the first run.
    if [[ "$run" -eq 1 ]]; then
        {
            echo "memory_pressure_after: $(memory_pressure 2>/dev/null | tail -1)"
            echo "ollama_ps_after: $(ollama ps 2>/dev/null | tail -n +2 | tr '\n' ';')"
        } >>"$SYSINFO"
    fi

    echo "  [run $run/$REPEAT] waiting for the game to finish its run..."
    wait "$GAME_PID" 2>/dev/null || true
    GAME_PID=""

    python3 "$SCRIPT_DIR/analyze.py" --frame-stats "$frames" --driver "$driver" \
        --scene "$MISSION" --label "${LABEL:-$MODEL}" --out "$report"
    REPORTS+=("$report.json")
}

for run in $(seq 1 "$REPEAT"); do
    run_once "$run"
done

# ── Aggregate (only meaningful for repeated runs) ─────────────────────────────────────────────
if [[ "$REPEAT" -gt 1 ]]; then
    echo "── aggregate of $REPEAT runs ──"
    python3 "$SCRIPT_DIR/aggregate.py" --reports "${REPORTS[@]}" \
        --out "$RESULTS/macos${LABEL:+_$LABEL}_${STAMP}_agg"
fi
