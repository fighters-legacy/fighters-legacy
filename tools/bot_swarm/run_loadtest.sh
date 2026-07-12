#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run_loadtest.sh — launch fl-server with a load-test config, run bot_swarm against it,
# and capture a JSON report. Mirrors tools/latency_analysis/measure_linux.sh.
#
# Usage: run_loadtest.sh [BUILD_DIR] [CLIENTS] [DURATION] [PATTERN] [-- <extra bot_swarm args>]
#   BUILD_DIR  build tree containing the binaries (default: build/debug)
#   CLIENTS    synthetic client count (default: 128)
#   DURATION   soak seconds (default: 30)
#   PATTERN    weave|level|aggressive|idle|random (default: weave)
#   --         everything after a literal `--` is forwarded verbatim to bot_swarm; this is how
#              the scale gate (scale_gate.py, #520) injects --assert-max-kbs / --assert-min-tick-hz
#              / --assert-max-tick-ms. With no `--` block the behavior is identical to before.
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

# Collect any trailing args after a literal `--` to forward to bot_swarm (e.g. --assert-* flags).
EXTRA_ARGS=()
shift "$(( $# < 4 ? $# : 4 ))" || true
if [[ "${1:-}" == "--" ]]; then
    shift
    EXTRA_ARGS=("$@")
fi

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
# FL_LOADTEST_REPORT lets a caller (scale_gate.py) pin the exact report path so it needn't guess the
# auto-generated name. Default: timestamped name under results/.
REPORT="${FL_LOADTEST_REPORT:-$RESULTS_DIR/loadtest_${CLIENTS}c_${PATTERN}_$(date -u +%Y%m%dT%H%M%SZ).json}"

# Headroom on max_peers so the harness can also probe past the requested count.
MAX_PEERS=$(( CLIENTS + 16 ))
[[ "$MAX_PEERS" -gt 1024 ]] && MAX_PEERS=1024

METRICS="$WORKDIR/server_tick.json"

# Optional entity-scale knobs (#573). FL_TEST_SPAWN_AI pre-spawns N server-side AI entities to stress
# the entity pool + SpatialIndex; FL_SNAPSHOT_BUDGET overrides the per-client snapshot byte budget.
# All default to "off"/unset so a normal run is byte-identical to before.
TEST_SPAWN_AI="${FL_TEST_SPAWN_AI:-0}"
TEST_SPAWN_SPREAD_KM="${FL_TEST_SPAWN_SPREAD_KM:-50}"
SNAPSHOT_BUDGET="${FL_SNAPSHOT_BUDGET:-}"

# #580 knobs: FL_TEST_SPAWN_MIX = weighted controller mix for the pre-spawned entities
# ("loiter:70,pursuit:20,patrol:10"); FL_TEST_PROJECTILE_RATE + FL_TEST_PROJECTILE_TTL_S =
# projectile spawn/reap churn. All default off so a normal run is byte-identical to before.
TEST_SPAWN_MIX="${FL_TEST_SPAWN_MIX:-}"
TEST_PROJECTILE_RATE="${FL_TEST_PROJECTILE_RATE:-}"
TEST_PROJECTILE_TTL_S="${FL_TEST_PROJECTILE_TTL_S:-3.0}"

# FL_LOADTEST_GOVERNOR=1 flips the graceful tick-overrun governor (#514) ON — used by the synthetic
# overrun profile (#574) to validate that the governor sheds under load. Default OFF, so the raw
# capacity gate still measures un-shed sim/bandwidth against the committed baseline.
# FL_LOADTEST_TRANSPORT (#649): transport BOTH ends speak. Default enet — bot_swarm is the enet6
# regression instrument and every pre-existing profile must keep its exact behaviour. "gns" needs an
# FL_ENABLE_GNS=ON build; bot_swarm hard-fails rather than silently falling back to enet6, so a GNS
# run that cannot speak GNS fails the gate instead of quietly measuring the wrong transport.
TRANSPORT="${FL_LOADTEST_TRANSPORT:-enet}"
if [[ "$TRANSPORT" != "enet" && "$TRANSPORT" != "gns" ]]; then
    echo "ERROR: FL_LOADTEST_TRANSPORT must be enet or gns (got '$TRANSPORT')"; exit 1
fi

GOVERNOR_ENABLED="false"
[[ "${FL_LOADTEST_GOVERNOR:-0}" == "1" ]] && GOVERNOR_ENABLED="true"

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

[world]
# The scale gate measures RAW sim/bandwidth capacity against the committed baseline, so the graceful
# tick-overrun governor (#514) is disabled here by default — otherwise it would shed snapshot/AI work
# under load and mask the very regressions the gate exists to catch. The synthetic-overrun profile
# (#574) flips it on via FL_LOADTEST_GOVERNOR=1 to validate the governor sheds. Defaults ON in prod.
overrun_governor_enabled = $GOVERNOR_ENABLED
# Entity-scale load-spawn (#573). 0 = disabled (normal run).
test_spawn_ai_count = $TEST_SPAWN_AI
test_spawn_spread_km = $TEST_SPAWN_SPREAD_KM
EOF
# Only emit snapshot_budget_bytes when explicitly requested (else keep the server default).
if [[ -n "$SNAPSHOT_BUDGET" ]]; then
    echo "snapshot_budget_bytes = $SNAPSHOT_BUDGET" >>"$CONFIG"
fi
# #580: only emit the mix/churn keys when requested (default runs stay byte-identical).
if [[ -n "$TEST_SPAWN_MIX" ]]; then
    echo "test_spawn_ai_mix = \"$TEST_SPAWN_MIX\"" >>"$CONFIG"
fi
if [[ -n "$TEST_PROJECTILE_RATE" ]]; then
    echo "test_projectile_rate = $TEST_PROJECTILE_RATE" >>"$CONFIG"
    echo "test_projectile_ttl_s = $TEST_PROJECTILE_TTL_S" >>"$CONFIG"
fi
cat >>"$CONFIG" <<EOF

[metrics]
tick_json_path = "$METRICS"
tick_json_interval_ms = 250
EOF

# FL_SIM_WORKER_THREADS sweeps the data-parallel sim worker count without editing config (#511/#573).
SIM_WORKER_ARGS=()
if [[ -n "${FL_SIM_WORKER_THREADS:-}" ]]; then
    SIM_WORKER_ARGS=(--sim-worker-threads "$FL_SIM_WORKER_THREADS")
fi

echo "=== bot_swarm load test: $CLIENTS clients, pattern=$PATTERN, ${DURATION}s, port $PORT" \
     "(test_spawn_ai=$TEST_SPAWN_AI mix=${TEST_SPAWN_MIX:-loiter} churn=${TEST_PROJECTILE_RATE:-0}/s" \
     "sim_workers=${FL_SIM_WORKER_THREADS:-default} governor=$GOVERNOR_ENABLED transport=$TRANSPORT) ==="
# Both ends are pinned to the SAME transport explicitly, overriding the [network].transport default:
# enet6 by default (bot_swarm is the enet6 regression instrument, #507/#519), gns for the #649 leg
# that validates the DEFAULT internet transport at scale.
FL_CONFIG="$CONFIG" "$FLSERVER" "$PORT" "$MAX_PEERS" --bind 127.0.0.1 --transport "$TRANSPORT" \
    ${SIM_WORKER_ARGS[@]+"${SIM_WORKER_ARGS[@]}"} &
SERVER_PID=$!

# Give the server a moment to bind, then confirm it is still alive.
sleep 2
kill -0 "$SERVER_PID" 2>/dev/null || { echo "ERROR: fl-server exited during startup"; exit 1; }

# Debug-only RSS sample (KiB) right after startup. The AUTHORITATIVE soak leak signal is now the
# server's self-reported server_tick.rss_kb / rss_startup_kb (#707), which scale_gate.py hard-gates
# via --assert-max-rss-growth-kb; this `ps` echo is retained only as a human diagnostic and is not
# Windows-portable. `|| true` keeps a flaky `ps` from tripping `set -euo pipefail`.
RSS_START="$(ps -o rss= -p "$SERVER_PID" 2>/dev/null | tr -d ' ' || true)"

# --server-metrics points bot_swarm at the file fl-server writes (above), so the report carries
# the authoritative per-phase server_tick block alongside the client-side proxy. Any EXTRA_ARGS
# (e.g. --assert-* from the scale gate) are forwarded verbatim.
set +e
"$BOTSWARM" 127.0.0.1 "$PORT" \
    --clients "$CLIENTS" --duration "$DURATION" --pattern "$PATTERN" --transport "$TRANSPORT" \
    --json "$REPORT" --server-metrics "$METRICS" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
STATUS=$?
set -e

# Debug-only: RSS again before teardown, and the delta. Diagnostic echo only — the gate reads the
# authoritative server_tick.rss_kb from the report, not this line.
RSS_END="$(ps -o rss= -p "$SERVER_PID" 2>/dev/null | tr -d ' ' || true)"
if [[ -n "$RSS_START" && -n "$RSS_END" ]]; then
    echo "[debug] server_rss_start_kb=$RSS_START server_rss_end_kb=$RSS_END server_rss_delta_kb=$(( RSS_END - RSS_START ))"
fi

# Sanity: the authoritative server-side block must be present in the report.
if ! grep -q '"server_tick"' "$REPORT"; then
    echo "ERROR: report $REPORT is missing the authoritative server_tick block"
    exit 1
fi

echo "Report: $REPORT"
exit $STATUS
