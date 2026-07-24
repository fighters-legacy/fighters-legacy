#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# measure_linux.sh — LLM inference vs Vulkan renderer GPU contention, Linux leg (#782).
#
# Boots a standalone fl-server, attaches the game as an observer ghost recording per-frame render
# timing (--frame-stats-json), drives a local LLM on a phased burst schedule alongside it, and
# joins the two by wall-clock timestamp to report what inference did to the frame time.
#
# Mirror pair: measure_macos.sh (Metal) and measure_windows.ps1 (CUDA + Vulkan llama.cpp). Keep
# the three in step — the whole value of the comparison is that the runs are the same run.
#
# Usage: measure_linux.sh [options] [BUILD_DIR]
#   --model <id>        model as the endpoint names it (default: qwen2.5-coder:14b)
#   --base-url <url>    OpenAI-compatible endpoint (default: $FL_AI_BASE_URL or ollama's)
#   --workload <name>   intent|mission|ops (default: intent — the wingman workload)
#   --mission <id>      scene to fly (default: builtin:sandbox)
#   --concurrency <n>   parallel request loops during a burst (default: 1)
#   --bursts <n>        burst count (default: 5)
#   --burst-seconds <n> seconds per burst (default: 20)
#   --idle-seconds <n>  baseline idle window before the first burst (default: 60)
#   --label <text>      suffix for the results filenames (e.g. cuda, vulkan)
#   BUILD_DIR           build tree with the binaries (default: build/release)
#
# Env: FL_CONTENTION_PORT (default 4796), FL_AI_BASE_URL, FL_AI_API_KEY.
#
# BEFORE RUNNING: pin the model in memory (`export OLLAMA_KEEP_ALIVE=-1`, or the equivalent for
# your server). An evicted model reloads INSIDE a burst — a 14B costs ~55 s to load (#769) — and
# that load would be measured as contention rather than as what it is.
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
    --label) LABEL="$2"; shift ;;
    -h | --help) sed -n '3,28p' "$0"; exit 0 ;;
    *) BUILD_DIR="$1" ;;
    esac
    shift
done

PORT="${FL_CONTENTION_PORT:-4796}"
GAME="$BUILD_DIR/game/fighters-legacy/fighters-legacy"
FLSERVER="$BUILD_DIR/server/fl-server/fl-server"
RESULTS="$SCRIPT_DIR/results"
STAMP="$(date -u +"%Y%m%dT%H%M%SZ")"
SUFFIX="linux${LABEL:+_$LABEL}_$STAMP"
mkdir -p "$RESULTS"

# ── Preflight ────────────────────────────────────────────────────────────────────────────────
[[ -x "$GAME" ]] || { echo "ERROR: game binary not found at $GAME (build a Release tree first)"; exit 1; }
[[ -x "$FLSERVER" ]] || { echo "ERROR: fl-server not found at $FLSERVER"; exit 1; }
curl -fsS --max-time 5 "$BASE_URL/v1/models" >/dev/null 2>&1 ||
    curl -fsS --max-time 5 "$BASE_URL/api/tags" >/dev/null 2>&1 ||
    { echo "ERROR: no OpenAI-compatible endpoint answering at $BASE_URL"; exit 1; }

# The driver owns the schedule arithmetic; the game's run length is derived from it rather than
# recomputed here, so the two cannot drift apart.
SCHEDULE_S="$(python3 "$SCRIPT_DIR/driver.py" --model "$MODEL" --print-schedule \
    --idle-seconds "$IDLE_SECONDS" --bursts "$BURSTS" --burst-seconds "$BURST_SECONDS" \
    --gap-seconds "$GAP_SECONDS" --tail-seconds "$TAIL_SECONDS")"
# Margin covers connect + terrain streaming before the first Flight frame, and the warm-up probe
# (which may be a full cold model load) that runs before the schedule starts.
RUN_SECONDS="$(python3 -c "import sys; print(int(float(sys.argv[1])) + 150)" "$SCHEDULE_S")"

echo "── GPU contention (#782), Linux ──"
echo "  endpoint : $BASE_URL   model: $MODEL   workload: $WORKLOAD (concurrency $CONCURRENCY)"
echo "  scene    : $MISSION    schedule: ${SCHEDULE_S}s   game run: ${RUN_SECONDS}s"
echo "  results  : $RESULTS/*_$SUFFIX.*"

# ── System info + VRAM before anything is loaded ─────────────────────────────────────────────
SYSINFO="$RESULTS/sysinfo_$SUFFIX.txt"
{
    echo "date_utc: $STAMP"
    echo "kernel: $(uname -srmo)"
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "mem_total_kb: $(awk '/MemTotal/ {print $2}' /proc/meminfo)"
    if command -v nvidia-smi >/dev/null 2>&1; then
        echo "gpu: $(nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader)"
        echo "vram_before_mb: $(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)"
        echo "compute_apps_before: $(nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader | tr '\n' ';')"
    else
        echo "gpu: nvidia-smi unavailable (non-NVIDIA or driver tools not installed)"
    fi
} >"$SYSINFO"
echo "  sysinfo  : $SYSINFO"

# ── Boot the server ──────────────────────────────────────────────────────────────────────────
WORKDIR="$(mktemp -d)"
SERVER_LOG="$WORKDIR/server.log"
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

# ── Launch the game ──────────────────────────────────────────────────────────────────────────
# Observer, not pilot: a spectator ghost holds a fixed camera over a streamed-in scene, so the
# render load is repeatable. A pilot aircraft flies off, changes what is on screen, and eventually
# hits the ground — none of which is a controlled baseline.
#
# Windowed, not --headless: present and the compositor are part of what contends for the GPU.
FRAMES_JSON="$RESULTS/frames_$SUFFIX.json"
"$GAME" --connect "127.0.0.1:$PORT" --observer --auto \
    --frame-stats-json "$FRAMES_JSON" --run-seconds "$RUN_SECONDS" >"$WORKDIR/game.log" 2>&1 &
GAME_PID=$!

echo "  waiting for the client to reach Flight and start recording..."
for _ in $(seq 1 150); do
    [[ -s "$FRAMES_JSON" ]] && break
    kill -0 "$GAME_PID" 2>/dev/null || { echo "ERROR: game exited early:"; tail -20 "$WORKDIR/game.log"; exit 1; }
    sleep 1
done
[[ -s "$FRAMES_JSON" ]] || { echo "ERROR: no frame stats after 150 s:"; tail -20 "$WORKDIR/game.log"; exit 1; }

# ── Drive the bursts ─────────────────────────────────────────────────────────────────────────
DRIVER_JSON="$RESULTS/driver_$SUFFIX.json"
python3 "$SCRIPT_DIR/driver.py" \
    --base-url "$BASE_URL" --model "$MODEL" --workload "$WORKLOAD" --concurrency "$CONCURRENCY" \
    --idle-seconds "$IDLE_SECONDS" --bursts "$BURSTS" --burst-seconds "$BURST_SECONDS" \
    --gap-seconds "$GAP_SECONDS" --tail-seconds "$TAIL_SECONDS" --out "$DRIVER_JSON"

# ── VRAM at peak load (model resident, game rendering), then tear down ────────────────────────
if command -v nvidia-smi >/dev/null 2>&1; then
    {
        echo "vram_after_mb: $(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)"
        echo "compute_apps_after: $(nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader | tr '\n' ';')"
    } >>"$SYSINFO"
fi

echo "  waiting for the game to finish its run..."
wait "$GAME_PID" 2>/dev/null || true

# ── Analyze ──────────────────────────────────────────────────────────────────────────────────
python3 "$SCRIPT_DIR/analyze.py" --frame-stats "$FRAMES_JSON" --driver "$DRIVER_JSON" --scene "$MISSION" \
    --out "$RESULTS/linux${LABEL:+_$LABEL}_$STAMP"
