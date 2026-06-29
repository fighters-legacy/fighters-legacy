#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run_loadtest.sh — launch fl-server with a load-test config, run bot_swarm against it,
# and capture a JSON report. Mirrors tools/latency_analysis/measure_linux.sh.
#
# Usage: run_loadtest.sh [BUILD_DIR] [CLIENTS] [DURATION] [PATTERN]
#   BUILD_DIR  build tree containing the binaries (default: build/debug)
#   CLIENTS    synthetic client count (default: 128)
#   DURATION   soak seconds (default: 30)
#   PATTERN    weave|level|aggressive|idle|random (default: weave)
#
# The server's connect-rate-limit and per-IP caps come ONLY from server.toml, so this writes
# a load-test config and points fl-server at it via FL_CONFIG (which never overwrites an
# existing file). Requires the raised server scale ceilings (max_peers up to 1024).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build/debug}"
CLIENTS="${2:-128}"
DURATION="${3:-30}"
PATTERN="${4:-weave}"
PORT="${FL_LOADTEST_PORT:-4793}"

FLSERVER="$BUILD_DIR/server/fl-server/fl-server"
BOTSWARM="$BUILD_DIR/tools/bot_swarm"
[[ -x "$FLSERVER" ]] || { echo "ERROR: fl-server not found at $FLSERVER"; exit 1; }
[[ -x "$BOTSWARM" ]] || { echo "ERROR: bot_swarm not found at $BOTSWARM"; exit 1; }

# Each client is a UDP socket; raise the open-file soft limit if we can.
ulimit -n "$(ulimit -Hn 2>/dev/null || echo 4096)" 2>/dev/null || true

WORKDIR="$(mktemp -d)"
trap 'kill "${SERVER_PID:-0}" 2>/dev/null || true; rm -rf "$WORKDIR"' EXIT
CONFIG="$WORKDIR/server.toml"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"
REPORT="$RESULTS_DIR/loadtest_${CLIENTS}c_${PATTERN}_$(date -u +%Y%m%dT%H%M%SZ).json"

# Headroom on max_peers so the harness can also probe past the requested count.
MAX_PEERS=$(( CLIENTS + 16 ))
[[ "$MAX_PEERS" -gt 1024 ]] && MAX_PEERS=1024

METRICS="$WORKDIR/server_tick.json"

cat >"$CONFIG" <<EOF
[server]
port = $PORT
bind_address = "127.0.0.1"
max_peers = $MAX_PEERS

[security]
connect_rate_limit_count = 100000
connect_rate_limit_window_s = 1
pre_handshake_rate_limit_count = 0
packet_flood_multiplier = 3
max_connections_per_ip = 0

[metrics]
tick_json_path = "$METRICS"
tick_json_interval_ms = 250
EOF

echo "=== bot_swarm load test: $CLIENTS clients, pattern=$PATTERN, ${DURATION}s, port $PORT ==="
FL_CONFIG="$CONFIG" "$FLSERVER" "$PORT" "$MAX_PEERS" --bind 127.0.0.1 &
SERVER_PID=$!

# Give the server a moment to bind, then confirm it is still alive.
sleep 2
kill -0 "$SERVER_PID" 2>/dev/null || { echo "ERROR: fl-server exited during startup"; exit 1; }

# --server-metrics points bot_swarm at the file fl-server writes (above), so the report carries
# the authoritative per-phase server_tick block alongside the client-side proxy.
"$BOTSWARM" 127.0.0.1 "$PORT" \
    --clients "$CLIENTS" --duration "$DURATION" --pattern "$PATTERN" \
    --json "$REPORT" --server-metrics "$METRICS"
STATUS=$?

# Sanity: the authoritative server-side block must be present in the report.
if ! grep -q '"server_tick"' "$REPORT"; then
    echo "ERROR: report $REPORT is missing the authoritative server_tick block"
    exit 1
fi

echo "Report: $REPORT"
exit $STATUS
