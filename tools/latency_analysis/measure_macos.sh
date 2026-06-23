#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ENet loopback latency measurement — macOS (kqueue)
#
# Prerequisites: iperf3 (brew install iperf3)
#   See docs/development.md — "Loopback latency analysis" section.
#
# Usage: bash tools/latency_analysis/measure_macos.sh [build_dir]
#   build_dir defaults to build/debug relative to repo root.
#
# Output: tools/latency_analysis/results/macos_<timestamp>.json
#   Run python3 tools/latency_analysis/compare.py to tabulate results.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build/debug}"
RESULTS_DIR="$SCRIPT_DIR/results"
TIMESTAMP=$(date -u +"%Y%m%dT%H%M%SZ")
REPORT="$RESULTS_DIR/macos_${TIMESTAMP}.json"
PORT=4779
BENCH_SAMPLES=600
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
OS=$(sw_vers -productVersion)
KERNEL=$(uname -r)
CPU=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || sysctl -n hw.model)
MAXSOCKBUF=$(sysctl -n kern.ipc.maxsockbuf)
CLOCKRATE=$(sysctl -n kern.clockrate 2>/dev/null | tr -d '{ }' || echo "N/A")

echo "=== System: macOS $OS ($KERNEL) ==="
echo "=== CPU: $CPU ==="
echo "=== kern.ipc.maxsockbuf=$MAXSOCKBUF ==="
echo "=== kern.clockrate=$CLOCKRATE ==="

# OS-level baseline — ICMP (macOS ping needs root for <200 ms interval; fall back gracefully)
echo ""
echo "--- ICMP ping baseline ---"
PING_SUMMARY=""
if sudo -n ping -c 100 -i 0.016 127.0.0.1 &>/dev/null 2>&1; then
    PING_SUMMARY=$(sudo ping -c 100 -i 0.016 127.0.0.1 2>&1 | grep "round-trip" | tail -1 || true)
else
    PING_SUMMARY=$(ping -c 100 -i 0.2 127.0.0.1 2>&1 | grep "round-trip" | tail -1 || true)
fi
echo "$PING_SUMMARY"

# OS-level baseline — UDP via iperf3
IPERF3_SUMMARY=""
if command -v iperf3 &>/dev/null; then
    echo ""
    echo "--- iperf3 UDP loopback baseline (10 s at 60 pps, 32-byte datagrams) ---"
    iperf3 -s -D --logfile /dev/null 2>/dev/null || true
    sleep 0.5
    IPERF3_SUMMARY=$(iperf3 -c 127.0.0.1 -u -b 60p --length 32 -t 10 2>&1 \
        | grep -E "sender|receiver" | tr '\n' ';' || true)
    pkill -f "iperf3 -s" 2>/dev/null || true
    echo "$IPERF3_SUMMARY"
else
    echo "WARN: iperf3 not found — skipping UDP baseline."
    echo "      Install: brew install iperf3  (see docs/development.md)"
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
  "platform": "macos",
  "os_version": "$OS",
  "kernel": "$KERNEL",
  "cpu": $(echo "$CPU" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))"),
  "kern_ipc_maxsockbuf": $MAXSOCKBUF,
  "kern_clockrate": $(echo "$CLOCKRATE" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))"),
  "bench_samples": $BENCH_SAMPLES,
  "bench_rate_hz": $BENCH_RATE,
  "icmp_summary": $(echo "$PING_SUMMARY" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))"),
  "iperf3_summary": $(echo "$IPERF3_SUMMARY" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))"),
  "bench_output": $(echo "$BENCH_OUT" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read().strip()))")
}
EOF

echo ""
echo "Report written to: $REPORT"
echo "Run: python3 tools/latency_analysis/compare.py  to tabulate all results."
