#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# visual_check.sh — one-command visual verification of the builtin placeholder meshes (#886).
#
# Default (observer) mode: boots a standalone fl-server with the builtin:shape-gallery mission
# (a museum row of every entity category + floating ordnance exhibits + a live fight 9 km out),
# stages wreck variants by injecting `detonate` admin commands into the server's stdin after a
# delay (so you can watch the intact -> wreck swap live), then opens the game window attached as
# an observer ghost — no menu interaction, no keypresses. Fly with WASD/QE, cycle entities with
# Num1/Num2 (Chase/Cockpit views via F2/F1, F4 back to free-fly), Escape/close to quit; the
# server is torn down automatically.
#
# Usage: visual_check.sh [--fly] [--build] [--mission <id>] [BUILD_DIR]
#   --fly          pilot mode instead: single-player into the gallery's armed player slot
#                  (embedded server) — fire bombs/rockets (stations 4/5) and strafe the museum
#                  row to make wrecks by hand. No detonate staging.
#   --build        run `cmake --build --preset debug` first.
#   --mission <id> mission to load (default: builtin:shape-gallery; accepts any builtin id,
#                  pack mission stem, or a .yaml file path — see fl-server MissionSource).
#   BUILD_DIR      build tree containing the binaries (default: build/debug)
#
# Env: FL_VISUAL_PORT (default 4795); FL_VISUAL_STAGE_DELAY seconds before the wreck-staging
# detonations fire (default 25; 0 disables staging).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FLY=0
BUILD=0
MISSION="builtin:shape-gallery"
BUILD_DIR="$REPO_ROOT/build/debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
    --fly) FLY=1 ;;
    --build) BUILD=1 ;;
    --mission)
        MISSION="$2"
        shift
        ;;
    *) BUILD_DIR="$1" ;;
    esac
    shift
done

PORT="${FL_VISUAL_PORT:-4795}"
STAGE_DELAY="${FL_VISUAL_STAGE_DELAY:-25}"

if [[ "$BUILD" == 1 ]]; then
    cmake --build --preset debug
fi

GAME="$BUILD_DIR/game/fighters-legacy/fighters-legacy"
FLSERVER="$BUILD_DIR/server/fl-server/fl-server"
[[ -x "$GAME" ]] || { echo "ERROR: game binary not found at $GAME (build first: $0 --build)"; exit 1; }

# ── Pilot mode: single-player straight into the gallery's armed player slot. ──────────────────
if [[ "$FLY" == 1 ]]; then
    echo "visual_check: launching pilot mode into $MISSION (fire bombs/rockets on stations 4/5)"
    exec "$GAME" --mission "$MISSION"
fi

# ── Observer mode: standalone server + staged wrecks + observer ghost client. ─────────────────
[[ -x "$FLSERVER" ]] || { echo "ERROR: fl-server not found at $FLSERVER (build first: $0 --build)"; exit 1; }

WORKDIR="$(mktemp -d)"
FIFO="$WORKDIR/server-stdin"
SERVER_LOG="$WORKDIR/server.log"
mkfifo "$FIFO"

cleanup() {
    # Graceful quit down the admin stdin first, then make sure the process is gone.
    { printf 'quit\n' >&3; } 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    sleep 0.5
    kill "${SERVER_PID:-0}" 2>/dev/null || true
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# The server's stdin reader dispatches admin commands line-by-line; the FIFO (held open on fd 3)
# is our scripted command channel. The client joins over the internet-MP transport, so the server
# keeps its default (GNS when built with FL_ENABLE_GNS, else enet6) — no --transport override.
"$FLSERVER" "$PORT" 8 --bind 127.0.0.1 --mission "$MISSION" <"$FIFO" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
exec 3>"$FIFO"

for _ in $(seq 1 50); do
    grep -q "listening on" "$SERVER_LOG" 2>/dev/null && break
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "ERROR: fl-server exited early:"; tail -5 "$SERVER_LOG"; exit 1; }
    sleep 0.2
done
grep -q "listening on" "$SERVER_LOG" || { echo "ERROR: fl-server never came up:"; tail -5 "$SERVER_LOG"; exit 1; }
echo "visual_check: fl-server up on 127.0.0.1:$PORT with $MISSION (log: $SERVER_LOG)"

# Wreck staging: after the client has had time to connect and see the museum intact, damage (not
# destroy) each persistent-category target so it swaps to its slumped wreck placeholder. Tuned to
# leave each at ~55-60% HP (Light damage): gv 200 HP, bunker 800, ship 4000; blasts are 60 m so
# the floating ordnance exhibits (>100 m away, 1 HP) are untouched.
if [[ "$STAGE_DELAY" != 0 && "$MISSION" == "builtin:shape-gallery" ]]; then
    (
        sleep "$STAGE_DELAY"
        printf 'detonate 600 555 -240 60 100\n' >&3   # ground vehicle -> wreck
        printf 'detonate 600 555 -120 60 400\n' >&3   # structure -> wreck
        printf 'detonate 700 555 220 60 2000\n' >&3   # naval vessel -> wreck
    ) &
    echo "visual_check: wreck-staging detonations fire in ${STAGE_DELAY}s (FL_VISUAL_STAGE_DELAY=0 to disable)"
fi

echo "visual_check: opening observer window (Num1/Num2 cycle entities, F2 chase, F4 free-fly)"
"$GAME" --connect "127.0.0.1:$PORT" --observer --auto
