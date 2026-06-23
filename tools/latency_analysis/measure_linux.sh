#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ENet loopback latency measurement — Fedora Linux (epoll)
#
# Prerequisites: sockperf (sudo dnf install sockperf)
#   See docs/development.md — "Loopback latency analysis" section.
#
# Usage: bash tools/latency_analysis/measure_linux.sh [build_dir]
#   build_dir defaults to build/debug relative to repo root.
#
# Output: tools/latency_analysis/results/linux_<timestamp>.json
#   Run python3 tools/latency_analysis/compare.py to tabulate results.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build/debug}"
RESULTS_DIR="$SCRIPT_DIR/results"
TIMESTAMP=$(date -u +"%Y%m%dT%H%M%SZ")
REPORT="$RESULTS_DIR/linux_${TIMESTAMP}.json"
PORT=4779
BENCH_SAMPLES=600 # 10 s at 60 Hz
BENCH_RATE=60

mkdir -p "$RESULTS_DIR"

FLSERVER="$BUILD_DIR/server/fl-server/fl-server"
NETCHK="$BUILD_DIR/tools/net_check/net_check"
[[ -x "$FLSERVER" ]] || { echo "ERROR: fl-server not found at $FLSERVER"; echo "Build first: cmake --build --preset debug"; exit 1; }
[[ -x "$NETCHK"  ]] || { echo "ERROR: net_check not found at $NETCHK";  exit 1; }

SERVER_PID=""
cleanup() { [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# System info
KERNEL=$(uname -r)
CPU=$(grep -m1 "model name" /proc/cpuinfo | cut -d: -f2 | xargs)
RMEM=$(cat /proc/sys/net/core/rmem_default)
WMEM=$(cat /proc/sys/net/core/wmem_default)

echo "=== System: Linux $KERNEL ==="
echo "=== CPU: $CPU ==="
echo "=== UDP buffers: rmem=$RMEM wmem=$WMEM ==="

# OS-level baseline — ICMP loopback
echo ""
echo "--- ICMP ping baseline ---"
PING_SUMMARY=""
if ping -c 100 -i 0.016 127.0.0.1 &>/dev/null 2>&1; then
    PING_SUMMARY=$(ping -c 100 -i 0.016 127.0.0.1 2>&1 | grep -E "rtt|round-trip" | tail -1 || true)
else
    PING_SUMMARY=$(ping -c 50 -i 0.1 127.0.0.1 2>&1 | grep -E "rtt|round-trip" | tail -1 || true)
fi
echo "$PING_SUMMARY"

# OS-level baseline — UDP via sockperf
SOCKPERF_SUMMARY=""
if command -v sockperf &>/dev/null; then
    echo ""
    echo "--- sockperf UDP loopback baseline (600 msg at 60 Hz, 32-byte payload) ---"
    sockperf server --ip 127.0.0.1 --port 11111 --daemonize --log-file /dev/null 2>/dev/null || true
    sleep 0.5
    SOCKPERF_SUMMARY=$(sockperf ping-pong --ip 127.0.0.1 --port 11111 \
        --time 11 --mps 60 --msg-size 32 2>&1 | grep -E "Summary|percentile|avg" | tr '\n' ';' || true)
    pkill -f "sockperf server" 2>/dev/null || true
    echo "$SOCKPERF_SUMMARY"
else
    echo "WARN: sockperf not found — skipping UDP baseline."
    echo "      Install: sudo dnf install sockperf  (see docs/development.md)"
fi

# Start fl-server
echo ""
echo "--- Starting fl-server on 127.0.0.1:$PORT ---"
"$FLSERVER" "$PORT" 1 --bind 127.0.0.1 --admin-token benchtoken &
SERVER_PID=$!
sleep 2

# ENet bench
echo ""
echo "--- net_check --bench $BENCH_SAMPLES --bench-rate $BENCH_RATE ---"
BENCH_OUT=$("$NETCHK" 127.0.0.1 "$PORT" --bench "$BENCH_SAMPLES" --bench-rate "$BENCH_RATE" 2>&1)
echo "$BENCH_OUT"

# Write JSON report
cat >"$REPORT" <<EOF
{
  "timestamp": "$TIMESTAMP",
  "platform": "linux",
  "kernel": "$KERNEL",
  "cpu": $(echo "$CPU" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))"),
  "udp_rmem": $RMEM,
  "udp_wmem": $WMEM,
  "bench_samples": $BENCH_SAMPLES,
  "bench_rate_hz": $BENCH_RATE,
  "icmp_summary": $(echo "$PING_SUMMARY" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))"),
  "sockperf_summary": $(echo "$SOCKPERF_SUMMARY" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))"),
  "bench_output": $(echo "$BENCH_OUT" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))")
}
EOF

echo ""
echo "Report written to: $REPORT"
echo "Run: python3 tools/latency_analysis/compare.py  to tabulate all results."
