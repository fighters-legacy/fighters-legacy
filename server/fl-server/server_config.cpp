// SPDX-License-Identifier: GPL-3.0-or-later
#include "server_config.h"
#include "TestSpawn.h" // parseTestSpawnMix — validates [world] test_spawn_ai_mix at parse time (#580)
#include "config/TomlNumeric.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <toml++/toml.hpp>

namespace fl {

// ---------------------------------------------------------------------------
// Default configuration template
// ---------------------------------------------------------------------------

static const char* kDefaultToml =
    "[server]\n"
    "# Human-readable server name shown in the lobby browser.\n"
    "name = \"Unnamed Server\"\n"
    "\n"
    "# UDP port fl-server binds on. Port 4778 is the fighters-legacy default.\n"
    "# See IANA registration note in docs/developer/architecture.md.\n"
    "port = 4778\n"
    "\n"
    "# Network interface to bind on.\n"
    "# \"::\"         = dual-stack all interfaces (IPv4+IPv6; recommended for internet servers)\n"
    "# \"0.0.0.0\"   = IPv4 all interfaces\n"
    "# \"127.0.0.1\" = localhost-only IPv4 (single-player; game client uses this)\n"
    "# \"::1\"        = localhost-only IPv6\n"
    "bind_address = \"0.0.0.0\"\n"
    "\n"
    "# Maximum number of simultaneous connected peers (1-1024).\n"
    "max_peers = 16\n"
    "\n"
    "# Scenario types this server will host.\n"
    "# Valid values: \"campaign\", \"mission\", \"sandbox\"\n"
    "game_modes = [\"campaign\", \"mission\", \"sandbox\"]\n"
    "\n"
    "# Message shown to connecting clients. Empty string = no message.\n"
    "motd = \"\"\n"
    "\n"
    "# Override client MOTD banner timeout (seconds). 0 = use client's motd_display_s setting.\n"
    "motd_display_s = 0\n"
    "\n"
    "# Server password. Empty string = no password required.\n"
    "password = \"\"\n"
    "\n"
    "[rotation]\n"
    "order = \"sequential\"\n"
    "# Each item is a mission ref, optionally with a game mode: \"mission@builtin:tdm\".\n"
    "items = []\n"
    "time_limit_min = 0\n"
    "\n"
    "[match]\n"
    "# Default game mode: a \"builtin:\" id (free-flight, tdm) or a pack modes/ asset stem.\n"
    "mode = \"builtin:free-flight\"\n"
    "# Seconds the end-of-match scoreboard shows (combat frozen) before rotating.\n"
    "end_screen_s = 10\n"
    "# Seconds a disconnected player's team + score are held for reconnect (0 = disabled).\n"
    "reconnect_grace_s = 120\n"
    "\n"
    "[bots]\n"
    "# Fill the match with AI bots up to this many total participants (0 = no bots).\n"
    "fill = 0\n"
    "max_bots = 16\n"
    "ai_script = \"builtin:fighter\"\n"
    "balance_teams = true\n"
    "\n"
    "[lobby]\n"
    "register = false\n"
    "url = \"https://lobby.fighters-legacy.org\"\n"
    "visibility = \"public\"\n"
    "\n"
    "[mods]\n"
    "stack = []\n"
    "# required = [\"fl-base\", \"theater@1.2\"]  # packs a client must have (\"id\" or \"id@version\") (#872)\n"
    "# required_policy = \"warn\"          # warn | refuse | allow_placeholder -- action on a missing pack\n"
    "\n"
    "[world]\n"
    "save_path = \"world.sav\"\n"
    "autosave_interval_s = 300\n"
    "# entity_soft_cap = 0              # ceiling on live world objects; 0 = unlimited (#1049).\n"
    "#                                  # Spawns past it are REFUSED (nothing is killed to make room),\n"
    "#                                  # so a runaway mission script or a stuck respawn loop cannot\n"
    "#                                  # exhaust memory. max_peers entities are reserved for player\n"
    "#                                  # airframes, so a world full of projectiles cannot lock pilots\n"
    "#                                  # out; refusals are logged and counted in `status`/metrics.\n"
    "#                                  # Hot-reloadable via reload_config. [0, INT_MAX]\n"
    "# player_entity_type = \"builtin:debug-entity\"  # aircraft a connecting pilot flies when the\n"
    "#                                  # client requests none; clamped to a registered type (#834).\n"
    "# allow_observers = true           # false = refuse observer-role (spectator) connections (#857).\n"
    "# planet_radius_m = 6371000        # planet sphere radius (m); Earth default\n"
    "# earth_rotation = true            # Coriolis + centrifugal in the Earth-fixed world frame (#482)\n"
    "# player_faction = 1               # faction stamped on every player entity on connect (#610).\n"
    "#                                  # MUST be non-zero for combat: faction 0 is NEUTRAL, and a\n"
    "#                                  # neutral entity has NO ENEMIES — nothing is hostile to a\n"
    "#                                  # player, so AI engage/cover logic never fires and the\n"
    "#                                  # wingman cannot designate a target. 0 = pre-#610 behaviour.\n"
    "#                                  # NOTE: with this set, a `spawn --faction 2 --ai escort` AI\n"
    "#                                  # now reacts to an approaching player, where before the\n"
    "#                                  # player was invisible to it. [0, 65535]\n"
    "# draw_distance_km = 100.0         # per-peer interest management radius (km); [1, 100000].\n"
    "#                                  # This is PRESENTATION relevance, not sensor truth: what a\n"
    "#                                  # player KNOWS arrives via the datalink, which is not bounded\n"
    "#                                  # by this radius. 100 km is the 128-client validated envelope\n"
    "#                                  # -- generous for visual and BVR presentation, and the real\n"
    "#                                  # limiter at scale is the per-snapshot byte budget, not this.\n"
    "# sensor_check_hz = 10.0           # sensor geometry checks/sec; the REFERENCE cadence every "
    "authored pod is tuned against; [1, 60]\n"
    "# spatial_cell_size_km = 0.0       # SpatialIndex cell size (km); 0 = AUTO from draw distance\n"
    "#                                  # (clamp(draw/32, 500 m, 10 km)), which is the default; [0, 1000]; "
    "restart\n"
    "# snapshot_budget_bytes = 1200     # per-client snapshot byte budget; 0 = unlimited; [0, 65535]\n"
    "# jitter_buffer_depth = 4          # per-peer input queue depth (ticks); global cap for adaptive sizing; [1, 32]\n"
    "# jitter_buffer_adapt_window = 60  # EWMA smoothing window in ticks; alpha = 1/window; [10, 3600]\n"
    "# jitter_buffer_hysteresis = 2     # resize dead-band in ticks; [0, 8]\n"
    "# jitter_buffer_jitter_multiplier = 2.0  # k factor: depth = ceil(ewma_delay + k*jitter); [0.0, 8.0]\n"
    "# congestion_enabled = true        # adaptive per-client send-rate / congestion response (#518)\n"
    "# congestion_min_send_hz = 10.0    # floor snapshot rate under congestion; [1, 60]\n"
    "# congestion_loss_threshold = 0.02 # ENet mean loss fraction that marks a peer congested; [0, 1]\n"
    "# congestion_budget_floor_bytes = 400  # never scale a set snapshot budget below this; [0, 65535]\n"
    "# overrun_governor_enabled = true  # graceful tick-overrun governor: shed work over budget (#514)\n"
    "# overrun_high_watermark = 0.90    # EWMA tick-ms / budget that triggers shedding; [0.1, 1.0]\n"
    "# overrun_low_watermark = 0.60     # recovery threshold (dead-band below high); [0.0, high)\n"
    "# overrun_min_snapshot_hz = 15.0   # floor broadcast rate under overrun; [1, 60]\n"
    "# overrun_max_ai_stride = 4        # deepest AI-sample decimation for non-player entities; [1, 32]\n"
    "# overrun_budget_floor_bytes = 400 # never scale the snapshot budget below this under overrun; [0, 65535]\n"
    "# overrun_min_interest_fraction = 0.5 # interest-radius floor fraction under overrun; [0.1, 1.0]; 1.0 = lever "
    "off\n"
    "# max_catchup_ticks = 8            # GameLoop catch-up cap (spiral backstop); [1, 64]; needs restart\n"
    "# sim_worker_threads = 0           # sim-tick CPU parallelism; 0 = auto, 1 = serial; [0, 256]\n"
    "# --- load-test affordance (#573): pre-spawn AI entities to stress the pool/index at scale. ---\n"
    "# --- A TESTING AFFORDANCE, NOT A CAPACITY GUARANTEE. Leave at 0 for normal operation. ---\n"
    "# test_spawn_ai_count = 0          # server-side AI entities to pre-spawn at startup; 0 = disabled; [0, 1000000]\n"
    "# test_spawn_spread_km = 50.0      # phyllotaxis spread radius (km); [0, 100000]\n"
    "# test_spawn_agl_m = 500.0         # spawn/loiter altitude above origin ground elevation (m); [0, 50000]\n"
    "# test_spawn_ai_mix = \"\"           # weighted controller mix, e.g. \"loiter:70,pursuit:20,patrol:10\"; empty = "
    "all loiter (#580)\n"
    "# test_spawn_entity_type = \"\"       # load AI type; empty = builtin:debug-entity; builtin:bomber = crewed AI "
    "(#980)\n"
    "# test_projectile_rate = 0.0       # short-lived entities spawned per second (churn); 0 = disabled; [0, 100000] "
    "(#580)\n"
    "# test_projectile_ttl_s = 3.0      # churned-entity lifetime (s); [0.05, 600] (#580)\n"
    "\n"
    "[ai]\n"
    "difficulty = \"pilot\"               # what the SERVER runs: scales AI radar range + reaction time; "
    "cadet|pilot|ace\n"
    "difficulty_floor = \"recruit\"\n"
    "\n"
    "[gameplay]\n"
    "# The damage gates (#626). Server-authoritative; hot-reloadable via reload_config.\n"
    "friendly_fire = false              # false = same-faction weapon damage is suppressed\n"
    "crash_damage = true                # false = ground impacts report but do not damage\n"
    "\n"
    "[discovery]\n"
    "# LAN server discovery beacon.\n"
    "enabled = true\n"
    "interval_ms = 2000\n"
    "# Answer A2S-style server-info queries (for the browser ping column) on query_port.\n"
    "query_enabled = true\n"
    "query_port = 0   # 0 = auto (game port + 1)\n"
    "\n"
    "[security]\n"
    "connect_rate_limit_count = 5\n"
    "connect_rate_limit_window_s = 10\n"
    "packet_flood_multiplier = 3\n"
    "banlist_path = \"\"\n"
    "allowlist_path = \"\"\n"
    "incoming_bandwidth_bps = 0\n"
    "outgoing_bandwidth_bps = 0\n"
    "\n"
    "# Operator password for authenticated admin commands from connected game clients.\n"
    "# Empty string (default) = network admin commands disabled (stdin pipe only).\n"
    "# For single-player, fl-server uses a per-session token passed via --admin-token.\n"
    "operator_password = \"\"\n"
    "\n"
    "# Pre-handshake CONNECT flood mitigation. Drops ENet CONNECT packets from any\n"
    "# source IP that exceeds pre_handshake_rate_limit_count attempts within the\n"
    "# pre_handshake_window_ms sliding window, before ENet peer state is allocated.\n"
    "# Set pre_handshake_rate_limit_count = 0 to disable.\n"
    "pre_handshake_rate_limit_count = 20\n"
    "pre_handshake_window_ms = 1000\n"
    "\n"
    "# Maximum simultaneous connections from a single IP address. 0 = unlimited (default).\n"
    "# Range [0, 1024]. Applies post-handshake, after the rate-limit check.\n"
    "max_connections_per_ip = 0\n"
    "\n"
    "# World-mutating request limits. A seat or team grant despawns and respawns an entity and\n"
    "# re-sends the whole entity-type table, so these bound how OFTEN a peer may ask, not how\n"
    "# fast it may type. Over the limit a request is dropped silently — a reply per rejected\n"
    "# packet would be the amplifier these exist to remove.\n"
    "seat_request_rate_limit_per_s = 2   # seat requests per second per player; [1, 60]\n"
    "team_switch_cooldown_s = 5          # seconds between accepted team switches; 0 = none; [0, 3600]\n"
    "# Heartbeats per second per player that draw a ping reply; [1, 60]. Excess heartbeats still\n"
    "# refresh liveness (a flooding peer cannot time itself out), they just go unanswered.\n"
    "heartbeat_rate_limit_per_s = 4\n"
    "\n"
    "# Per-IP lockout for the operator network admin channel (MsgAdminCommand).\n"
    "# After admin_auth_max_failures consecutive wrong passwords from the same IP the peer\n"
    "# is kicked and reconnections from that IP are refused for admin_auth_lockout_s seconds.\n"
    "# Range: max_failures [1,100], lockout_s [1,86400].\n"
    "admin_auth_max_failures = 5\n"
    "admin_auth_lockout_s = 300\n"
    "\n"
    "# Disconnect peers that send no MsgClientInput or MsgHeartbeat for this many seconds.\n"
    "# 0 = disabled (default). Recommended: 60-300 for public servers. Range: [0,86400].\n"
    "idle_timeout_s = 0\n"
    "\n"
    "[shutdown]\n"
    "shutdown_warning_interval_s = 300\n"
    "min_shutdown_delay_s = 0\n"
    "shutdown_require_confirm = true\n"
    "\n"
    "[http_admin]\n"
    "# REST admin API + /health probe (#233). Disabled by default.\n"
    "# Bound to localhost: exposing it to the network is a deliberate second edit.\n"
    "# Tokens travel as plain Bearer credentials over plain HTTP -- put this behind a\n"
    "# TLS-terminating reverse proxy, or keep it on the loopback for k8s/compose probes.\n"
    "# Enabling this with no tokens is refused, not warned: it would be an open admin API.\n"
    "enabled = false\n"
    "port = 8080\n"
    "bind_address = \"127.0.0.1\"\n"
    "max_auth_failures = 5\n"
    "lockout_seconds = 300\n"
    "# [[http_admin.tokens]]\n"
    "# token = \"change-me\"\n"
    "# role  = \"admin\"   # admin | moderator | gm | faction_leader\n"
    "# faction = -1        # faction index for a faction-scoped role; -1 = unbound\n"
    "# autonomy = \"\"     # MCP tier override: observe | recommend | act. Empty = [ai.mcp] autonomy\n"
    "\n"
    "[ai.mcp]\n"
    "# Model Context Protocol surface (#601). A SECOND FRONTEND on the [http_admin] listener above --\n"
    "# same port, same token table, same per-IP lockout -- so this needs http_admin enabled too.\n"
    "# Streamable HTTP: POST <path> for calls, GET <path> for the notification stream.\n"
    "# Tools: world_state, events (observe) | submit_mission (recommend) | admin_command (act).\n"
    "# A tool call is ALSO checked against the token's role capabilities, so 'act' is a ceiling and\n"
    "# not a bypass: an act-tier moderator token still cannot shut the server down.\n"
    "enabled = false\n"
    "path = \"/mcp\"\n"
    "# Default tier for a token row that does not set its own. Read-only by default.\n"
    "autonomy = \"observe\"\n"
    "# Commands an act-tier token may run. EMPTY PERMITS NOTHING -- list them explicitly.\n"
    "allowlist = []\n"
    "rate_limit_per_min = 120\n"
    "max_sessions = 32\n"
    "\n"
    "[ai.provider]\n"
    "# Generative-AI provider seam (#163): missions, campaign events, narrative, faction decisions,\n"
    "# and the free-text wingman intent tier. OPT-IN -- with this off (the default) every AI feature\n"
    "# degrades to its scripted path, which is the CI-tested one.\n"
    "enabled = false\n"
    "# Path to an IWorldAiProvider shared library. Empty = the built-in NullAiProvider (supports\n"
    "# nothing, and says so, so callers degrade by asking rather than by discovering).\n"
    "plugin = \"\"\n"
    "endpoint = \"\"\n"
    "model = \"\"\n"
    "# The NAME of the environment variable holding the API key -- never the key itself. A key in\n"
    "# this file gets committed; a key on a command line shows up in `ps`.\n"
    "api_key_env = \"FL_AI_API_KEY\"\n"
    "max_calls_per_minute = 10\n"
    "world_evolution_interval_min = 60\n"
    "\n"
    "[ai.chat_intent]\n"
    "# Free-text wingman commands over TEAM chat (#611). Needs [ai.provider] with the `intent`\n"
    "# capability; without one the in-game radio menu is the path (decision #769).\n"
    "# The model only ever CHOOSES among the six scripted commands -- it never invents an action and\n"
    "# never supplies a target, so a successful prompt injection buys \"a real command at the wrong\n"
    "# time\", which is what pressing a menu key would also buy.\n"
    "enabled = false\n"
    "# Model calls per minute per peer. Much lower than the chat rate limit: a chat line is free and\n"
    "# a model call is not.\n"
    "rate_limit_per_min = 6\n"
    "notify_on_decline = true\n"
    "\n"
    "[rcon]\n"
    "# Source Engine RCON (TCP) remote admin channel. Disabled by default.\n"
    "# Set a strong password before enabling. Password travels over plain TCP;\n"
    "# use only on trusted/VPN networks or behind a TLS-terminating reverse proxy.\n"
    "enabled = false\n"
    "port = 27015\n"
    "password = \"\"\n"
    "max_auth_failures = 5\n"
    "lockout_seconds = 60\n"
    "\n"
    "[metrics]\n"
    "# Per-phase server tick-budget JSON export. Empty path = disabled. Written atomically each\n"
    "# interval; consumed by bot_swarm (--server-metrics) and any external scraper. See\n"
    "# docs/developer/load-testing.md for the JSON schema.\n"
    "tick_json_path = \"\"\n"
    "tick_json_interval_ms = 1000\n"
    "\n"
    "[wind]\n"
    "# Altitude wind profile (#489). Empty = disabled (a single datum-level wind is used). Set a path\n"
    "# (relative to this config's directory) to a profile TOML generated by tools/gen_wind_profile.py:\n"
    "# a series of [[wind.profile]] knots {altitude_m, speed_ms, heading_deg}. Aircraft then feel the\n"
    "# wind at their altitude, and the client predicts it in parity.\n"
    "profile_path = \"\"\n"
    "\n"
    "[trace]\n"
    "# Server-side input tracing (#560). When input_trace_dir is non-empty, every peer's accepted\n"
    "# MsgClientInput is recorded to a per-peer FLIT trace (trace_peer<id>_<n>.flit) in that\n"
    "# directory, capturing real sessions for bot_swarm `--pattern trace:<file>` replay and the\n"
    "# Phase 4 replay epic. Empty = disabled. The trace_start [dir] / trace_stop admin commands\n"
    "# toggle it at runtime. See docs/developer/load-testing.md for the trace format.\n"
    "input_trace_dir = \"\"\n"
    "\n"
    "[replay]\n"
    "# Server-side match recording to the .flrep replay format (#643, docs/developer/replay-format.md). Every\n"
    "# match is recorded automatically; the client plays these back with full camera control and a\n"
    "# scrubbable timeline. Disk use is bounded by rotation, not by remembering to clean up.\n"
    "enabled = true\n"
    "# Directory for recordings, relative to the server's working directory.\n"
    "dir = \"replays\"\n"
    "# Ticks between keyframes. A scrub seeks to the keyframe at or before the target and rolls\n"
    "# forward, so this is the seek granularity. Measured: across 60-600 ticks the file size moves\n"
    "# by well under 1 %, so pick this for seek feel, not for disk. 120 = 2 s at 60 Hz. [15, 3600].\n"
    "keyframe_interval_ticks = 120\n"
    "# Rotate to a new file once the current one passes this size. [1, 65535] MB.\n"
    "max_file_mb = 256\n"
    "# Keep at most this many .flrep files in `dir`, deleting the oldest first. Bounds the DIRECTORY,\n"
    "# not one session. [1, 10000].\n"
    "max_files = 20\n"
    "# Per-tick state-hash sidecar for the determinism gate (#644). Empty = not written; a live\n"
    "# server has no use for it.\n"
    "hash_log = \"\"\n"
    "\n"
    "[network]\n"
    "# Transport backend: \"gns\" (GameNetworkingSockets — encrypted UDP, congestion control, 128+\n"
    "# headroom; default for dedicated servers) or \"enet\" (enet6 — LAN / low-count). See\n"
    "# docs/developer/decisions/transport-selection.md. Requires a server restart to change.\n"
    "transport = \"gns\"\n"
    "# GNS only: accept unauthenticated peers (standalone GNS has no Steam PKI). true = encrypted but\n"
    "# unauthenticated (opportunistic, like TLS-without-cert). Ignored by the enet backend.\n"
    "allow_insecure = true\n"
    "# zstd-compress snapshot payloads at the engine layer (#775). Transport-agnostic: enet6's own\n"
    "# range coder compressed on the wire, GNS (the default) does not compress at all — this makes\n"
    "# the shipping transport pay enet6-or-better wire bytes. Hot-reloadable via reload_config.\n"
    "compress_snapshots = true\n"
    "# GNS only: datagram-coalescing (Nagle) window in microseconds. 0 = GNS default (5000). Larger\n"
    "# values merge more acks/messages per datagram at the cost of up to that much added delivery\n"
    "# latency. Range [0, 200000]. Requires a server restart.\n"
    "gns_nagle_time_us = 0\n"
    "\n"
    "[spawn]\n"
    "# AGL offset (metres) above terrain for all spawn points. Default 500 m.\n"
    "agl_offset = 500.0\n"
    "\n"
    "# Peer spawn locations (round-robin). Terrain height is queried at each point\n"
    "# on startup and cached; changing spawn points requires a server restart.\n"
    "# Omit this section to use the default (origin).\n"
    "#\n"
    "# [[spawn.points]]\n"
    "# x = 0.0\n"
    "# z = 0.0\n"
    "\n"
    "[flight]\n"
    "# AI wingmen spawned for each connecting player (#610). 0 = players fly alone.\n"
    "#\n"
    "# Deliberately 0 by default: N extra AI entities per peer would move every load-test and\n"
    "# scale-gate number, so a dedicated server is unchanged unless you ask for a flight.\n"
    "# Single-player is unaffected — the game client starts its embedded server with\n"
    "# --flight-size 1, so you always fly with a wingman there.\n"
    "#\n"
    "# Larger structures — all-AI flights, strike packages, an AWACS commanding aircraft it is\n"
    "# not flying with — are built at runtime with the `flight` admin command, not here.\n"
    "size = 0\n"
    "# entity_type = \"builtin:debug-entity\"  # entity spawned for each wingman\n"
    "#\n"
    "# Formation geometry: slot spacing per rank. Members alternate right/left and step\n"
    "# out, back and down each rank, so any flight size stacks into a legible echelon.\n"
    "# lateral_m = 150.0                #  [10, 5000]\n"
    "# aft_m = 100.0                    #  [0, 5000]\n"
    "# vertical_m = -15.0               #  negative = stepped down; [-1000, 1000]\n"
    "#\n"
    "# Behaviour tuning for the six scripted commands.\n"
    "# engage_range_m = 12000.0         # engage_bandits trigger radius about the wingman; [500, 200000]\n"
    "# cover_range_m = 6000.0           # cover_me trigger radius about the LEAD; [500, 200000]\n"
    "# designate_range_m = 15000.0      # attack_my_target boresight range; [500, 200000]\n"
    "# designate_half_angle_deg = 15.0  # boresight cone half-angle; [1, 90]\n"
    "# command_rate_limit_per_s = 4     # wingman orders per second per player; [1, 60]\n"
    "\n"
    "[atc]\n"
    "# Air-traffic control (#673): runway sequencing, clearances, and a player comms menu.\n"
    "# Disable to boot with no facilities — radio commands then answer \"no ATC available\".\n"
    "enabled = true\n"
    "# scramble_entity_type = \"builtin:debug-entity\"  # default type for atc_scramble / atc.scramble\n"
    "\n"
    "[chat]\n"
    "# In-match text chat (#646): all/team channels, per-peer rate limit, admin mute.\n"
    "enabled = true\n"
    "# rate_limit_per_s = 2     # chat lines per second per player; [1, 60]\n"
    "\n"
    "[voice]\n"
    "# In-game voice comms (Epic J): PTT-keyed radio NETS, Opus relayed without server-side decode.\n"
    "# There is deliberately no frequency dial - nets are named channels.\n"
    "enabled = true\n"
    "# frame_rate_limit = 60    # frames/s per player; [1, 200]. 50 = one continuous transmission.\n"
    "#\n"
    "# Omit [[voice.nets]] entirely to use the compiled-in stack: team (default PTT), flight, atc,\n"
    "# and a positional proximity net at 3 km. Defining ANY net replaces that stack wholesale.\n"
    "# [[voice.nets]]\n"
    "# id = \"team\"            # addressed by config and admin commands; must be unique\n"
    "# name = \"TEAM\"          # display label on the HUD and PTT selector\n"
    "# kind = \"team\"          # global | team | flight | proximity | atc\n"
    "# default = true          # pre-selected under the primary PTT key\n"
    "# [[voice.nets]]\n"
    "# id = \"proximity\"\n"
    "# kind = \"proximity\"\n"
    "# positional = true       # mix at the speaker's bearing rather than head-locked\n"
    "# range_m = 3000.0        # audible radius; 0 = unlimited\n";

std::string_view defaultServerConfigToml() {
    return kDefaultToml;
}

static constexpr const char* kValidGameModes[] = {"campaign", "mission", "sandbox"};
static constexpr const char* kValidRotationOrder[] = {"sequential", "random"};
static constexpr const char* kValidVisibility[] = {"public", "private"};
static constexpr const char* kValidDifficulties[] = {"recruit", "cadet", "veteran", "ace"};

static bool isOneOf(const char* val, const char* const* arr, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (std::strcmp(val, arr[i]) == 0)
            return true;
    return false;
}

// No-op ILogger used when the caller passes a null logger (the parameter is documented as optional).
// Routing through this sink keeps the 50+ log->log(...) diagnostic sites below from dereferencing a
// null pointer on any config that triggers a validation warning or a parse error.
namespace {
struct NullLogSink final : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Read a double key, accept it only inside [lo, hi], and Warn + keep the existing default otherwise.
// This is the range-validated-default contract every key in this file follows; it is spelled out by
// hand dozens of times above, and new sections should not add a dozen more.
void clampDouble(const toml::table& tbl, const char* section, const char* key, double lo, double hi, double& out,
                 ILogger* log) {
    auto v = tbl[section][key].template value<double>();
    if (!v)
        return;
    if (*v >= lo && *v <= hi) {
        out = *v;
        return;
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s.%s out of range [%g, %g]; using default %g", section, key, lo, hi, out);
    log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
}
} // namespace

ServerConfig parseServerConfig(std::string_view content, ILogger* log) {
    static NullLogSink nullSink;
    if (!log)
        log = &nullSink;

    ServerConfig cfg;
    try {
        auto tbl = toml::parse(content);

        // [server]
        if (auto v = tbl["server"]["name"].value<std::string>())
            cfg.name = std::move(*v);
        if (auto v = tomlInt(tbl["server"]["port"])) {
            if (*v < 1 || *v > 65535) {
                log->log(LogLevel::Warn, __FILE__, __LINE__, "server.port out of range [1,65535]; using default");
            } else {
                cfg.port = static_cast<uint16_t>(*v);
            }
        }
        if (auto v = tbl["server"]["bind_address"].value<std::string>())
            cfg.bindAddress = std::move(*v);
        if (auto v = tomlInt(tbl["server"]["max_peers"])) {
            if (*v < 1 || *v > 1024) {
                log->log(LogLevel::Warn, __FILE__, __LINE__, "server.max_peers out of range [1,1024]; using default");
            } else {
                cfg.maxPeers = static_cast<int>(*v);
            }
        }
        if (auto* arr = tbl["server"]["game_modes"].as_array()) {
            std::vector<std::string> modes;
            for (auto& elem : *arr) {
                if (auto s = elem.value<std::string>()) {
                    if (isOneOf(s->c_str(), kValidGameModes, 3)) {
                        modes.push_back(std::move(*s));
                    } else {
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "server.game_modes: unknown value \"%s\"; skipping",
                                      s->c_str());
                        log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
                    }
                }
            }
            if (!modes.empty())
                cfg.gameModes = std::move(modes);
        }
        if (auto v = tbl["server"]["motd"].value<std::string>())
            cfg.motd = std::move(*v);
        if (auto v = tomlInt(tbl["server"]["motd_display_s"])) {
            int64_t clamped = std::clamp(*v, int64_t{0}, int64_t{65535});
            if (clamped != *v)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "server.motd_display_s out of range; clamped to [0, 65535]");
            cfg.motdDisplayS = static_cast<uint16_t>(clamped);
        }
        if (auto v = tbl["server"]["password"].value<std::string>())
            cfg.password = std::move(*v);

        // [rotation]
        if (auto v = tbl["rotation"]["order"].value<std::string>()) {
            if (isOneOf(v->c_str(), kValidRotationOrder, 2)) {
                cfg.rotationOrder = std::move(*v);
            } else {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "rotation.order must be \"sequential\" or \"random\"; using default");
            }
        }
        if (auto* arr = tbl["rotation"]["items"].as_array()) {
            for (auto& elem : *arr)
                if (auto s = elem.value<std::string>())
                    cfg.rotationItems.push_back(std::move(*s));
        }
        if (auto v = tomlInt(tbl["rotation"]["time_limit_min"]))
            cfg.rotationTimeLimitMin = static_cast<int>(*v);

        // [match] (#521/#523)
        if (auto v = tbl["match"]["mode"].value<std::string>())
            cfg.matchMode = std::move(*v);
        if (auto v = tomlInt(tbl["match"]["end_screen_s"])) {
            if (*v >= 0 && *v <= 120)
                cfg.matchEndScreenS = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__, "match.end_screen_s out of range [0,120]; using default");
        }
        if (auto v = tomlInt(tbl["match"]["reconnect_grace_s"])) {
            if (*v >= 0 && *v <= 3600)
                cfg.matchReconnectGraceS = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "match.reconnect_grace_s out of range [0,3600]; using default");
        }

        // [bots] (#87)
        if (auto v = tomlInt(tbl["bots"]["fill"])) {
            if (*v >= 0 && *v <= 128)
                cfg.botsFill = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__, "bots.fill out of range [0,128]; using default");
        }
        if (auto v = tomlInt(tbl["bots"]["max_bots"])) {
            if (*v >= 0 && *v <= 127)
                cfg.botsMax = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__, "bots.max_bots out of range [0,127]; using default");
        }
        if (auto v = tbl["bots"]["entity_type"].value<std::string>())
            cfg.botsEntityType = std::move(*v);
        if (auto v = tbl["bots"]["ai_script"].value<std::string>())
            cfg.botsAiScript = std::move(*v);
        if (auto v = tbl["bots"]["balance_teams"].value<bool>())
            cfg.botsBalanceTeams = *v;

        // [lobby]
        if (auto v = tbl["lobby"]["register"].value<bool>())
            cfg.lobbyRegister = *v;
        if (auto v = tbl["lobby"]["url"].value<std::string>())
            cfg.lobbyUrl = std::move(*v);
        if (auto v = tbl["lobby"]["visibility"].value<std::string>()) {
            if (isOneOf(v->c_str(), kValidVisibility, 2)) {
                cfg.lobbyVisibility = std::move(*v);
            } else {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "lobby.visibility must be \"public\" or \"private\"; using default");
            }
        }

        // [mods]
        if (auto* arr = tbl["mods"]["stack"].as_array()) {
            for (auto& elem : *arr)
                if (auto s = elem.value<std::string>())
                    cfg.modStack.push_back(std::move(*s));
        }
        if (auto* arr = tbl["mods"]["required"].as_array()) { // #872 required-pack specs ("id" / "id@version")
            for (auto& elem : *arr)
                if (auto s = elem.value<std::string>())
                    cfg.requiredPacks.push_back(std::move(*s));
        }
        if (auto v = tbl["mods"]["required_policy"].value<std::string>()) { // #872 warn / refuse / allow_placeholder
            static const char* const kValidPolicy[] = {"warn", "refuse", "allow_placeholder"};
            if (isOneOf(v->c_str(), kValidPolicy, 3)) {
                cfg.requiredPackPolicy = std::move(*v);
            } else {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "mods.required_policy must be \"warn\", \"refuse\", or \"allow_placeholder\"; using \"warn\"");
            }
        }

        // [world]
        if (auto v = tbl["world"]["save_path"].value<std::string>())
            cfg.worldSavePath = std::move(*v);
        if (auto v = tbl["world"]["player_entity_type"].value<std::string>())
            cfg.playerEntityType = std::move(*v); // clamped to a registered type at spawn (#834)
        if (auto v = tbl["world"]["allow_observers"].value<bool>())
            cfg.allowObservers = *v; // #857
        if (auto v = tomlInt(tbl["world"]["autosave_interval_s"]))
            cfg.worldAutosaveIntervalS = static_cast<int>(*v);
        if (auto v = tomlInt(tbl["world"]["entity_soft_cap"])) {
            if (*v < 0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__, "world.entity_soft_cap must be >= 0; using 0 (unlimited)");
            } else {
                cfg.entitySoftCap = static_cast<int>(*v);
            }
        }
        if (auto v = tbl["world"]["time_scale"].value<double>()) {
            if (*v <= 0.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__, "world.time_scale must be > 0; using default 10.0");
            } else {
                cfg.timeScale = *v;
            }
        }
        if (auto v = tomlInt(tbl["world"]["player_faction"])) {
            if (*v >= 0 && *v <= 65535) {
                cfg.playerFaction = static_cast<uint16_t>(*v);
            } else {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.player_faction out of range [0, 65535]; using default 1");
            }
        }
        if (auto v = tbl["world"]["planet_radius_m"].value<double>()) {
            if (*v < 1000.0 || *v > 1e9) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.planet_radius_m out of range [1000, 1e9]; using default 6371000.0");
            } else {
                cfg.planetRadiusM = *v;
            }
        }
        if (auto v = tbl["world"]["earth_rotation"].value<bool>()) {
            cfg.earthRotation = *v;
        }
        if (auto v = tbl["world"]["draw_distance_km"].value<double>()) {
            if (*v < 1.0 || *v > 100'000.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.draw_distance_km out of range [1, 100000]; using default 100.0");
            } else {
                cfg.drawDistanceKm = *v;
            }
        }
        if (auto v = tomlInt(tbl["world"]["spectate_delay_s"])) {
            if (*v >= 0 && *v <= 300)
                cfg.spectateDelayS = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.spectate_delay_s out of range [0, 300]; using default 0");
        }
        if (auto v = tbl["world"]["sensor_check_hz"].value<double>()) {
            if (*v < 1.0 || *v > 60.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.sensor_check_hz out of range [1, 60]; using default 10.0");
            } else {
                cfg.sensorCheckHz = *v;
            }
        }
        if (auto v = tbl["world"]["spatial_cell_size_km"].value<double>()) {
            if (*v < 0.0 || *v > 1'000.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.spatial_cell_size_km out of range [0, 1000]; using default 0.0 (auto)");
            } else {
                cfg.spatialCellSizeKm = *v;
            }
        }
        if (auto v = tomlInt(tbl["world"]["snapshot_budget_bytes"])) {
            if (*v < int64_t{0} || *v > int64_t{65535}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.snapshot_budget_bytes out of range [0, 65535]; using default 1200");
            } else {
                cfg.snapshotBudgetBytes = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["jitter_buffer_depth"])) {
            if (*v < int64_t{1} || *v > int64_t{32}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.jitter_buffer_depth out of range [1, 32]; using default 4");
            } else {
                cfg.jitterBufferDepth = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["jitter_buffer_adapt_window"])) {
            if (*v < int64_t{10} || *v > int64_t{3600}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.jitter_buffer_adapt_window out of range [10, 3600]; using default 60");
            } else {
                cfg.jitterAdaptWindow = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["jitter_buffer_hysteresis"])) {
            if (*v < int64_t{0} || *v > int64_t{8}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.jitter_buffer_hysteresis out of range [0, 8]; using default 2");
            } else {
                cfg.jitterHysteresis = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tbl["world"]["jitter_buffer_jitter_multiplier"].value<double>()) {
            if (*v < 0.0 || *v > 8.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.jitter_buffer_jitter_multiplier out of range [0.0, 8.0]; using default 2.0");
            } else {
                cfg.jitterMultiplier = static_cast<float>(*v);
            }
        }
        if (auto v = tbl["world"]["congestion_enabled"].value<bool>()) {
            cfg.congestionEnabled = *v;
        }
        if (auto v = tbl["world"]["congestion_min_send_hz"].value<double>()) {
            if (*v < 1.0 || *v > 60.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.congestion_min_send_hz out of range [1, 60]; using default 10.0");
            } else {
                cfg.congestionMinSendHz = static_cast<float>(*v);
            }
        }
        if (auto v = tbl["world"]["congestion_loss_threshold"].value<double>()) {
            if (*v < 0.0 || *v > 1.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.congestion_loss_threshold out of range [0, 1]; using default 0.02");
            } else {
                cfg.congestionLossThreshold = static_cast<float>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["congestion_budget_floor_bytes"])) {
            if (*v < int64_t{0} || *v > int64_t{65535}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.congestion_budget_floor_bytes out of range [0, 65535]; using default 400");
            } else {
                cfg.congestionBudgetFloorBytes = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["sim_worker_threads"])) {
            if (*v < int64_t{0} || *v > int64_t{256}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.sim_worker_threads out of range [0, 256]; using default 0 (auto)");
            } else {
                cfg.simWorkerThreads = static_cast<uint32_t>(*v);
            }
        }
        // Load-test affordance (#573): pre-spawn N server-side AI entities.
        if (auto v = tomlInt(tbl["world"]["test_spawn_ai_count"])) {
            if (*v < int64_t{0} || *v > int64_t{1'000'000}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.test_spawn_ai_count out of range [0, 1000000]; using default 0 (disabled)");
            } else {
                cfg.testSpawnAiCount = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tbl["world"]["test_spawn_spread_km"].value<double>()) {
            if (*v < 0.0 || *v > 100'000.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.test_spawn_spread_km out of range [0, 100000]; using default 50.0");
            } else {
                cfg.testSpawnSpreadKm = *v;
            }
        }
        if (auto v = tbl["world"]["test_spawn_agl_m"].value<double>()) {
            if (*v < 0.0 || *v > 50'000.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.test_spawn_agl_m out of range [0, 50000]; using default 500.0");
            } else {
                cfg.testSpawnAglM = *v;
            }
        }
        // #580: controller mix + projectile churn for the load-spawn.
        if (auto v = tbl["world"]["test_spawn_ai_mix"].value<std::string>()) {
            std::vector<TestSpawnMixEntry> mix;
            std::string mixErr;
            if (v->empty() || parseTestSpawnMix(*v, mix, mixErr)) {
                cfg.testSpawnAiMix = std::move(*v);
            } else {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         ("world.test_spawn_ai_mix invalid (" + mixErr + "); using all-loiter default").c_str());
            }
        }
        if (auto v = tbl["world"]["test_spawn_entity_type"].value<std::string>())
            cfg.testSpawnEntityType = std::move(*v); // #980: e.g. builtin:bomber for crewed-AI load
        if (auto v = tbl["world"]["test_projectile_rate"].value<double>()) {
            if (*v < 0.0 || *v > 100'000.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.test_projectile_rate out of range [0, 100000]; using default 0 (disabled)");
            } else {
                cfg.testProjectileRate = *v;
            }
        }
        if (auto v = tbl["world"]["test_projectile_ttl_s"].value<double>()) {
            if (*v < 0.05 || *v > 600.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.test_projectile_ttl_s out of range [0.05, 600]; using default 3.0");
            } else {
                cfg.testProjectileTtlS = *v;
            }
        }
        // Graceful tick-overrun governor (#514).
        if (auto v = tbl["world"]["overrun_governor_enabled"].value<bool>()) {
            cfg.overrunGovernorEnabled = *v;
        }
        if (auto v = tbl["world"]["overrun_high_watermark"].value<double>()) {
            if (*v < 0.1 || *v > 1.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.overrun_high_watermark out of range [0.1, 1.0]; using default 0.90");
            } else {
                cfg.overrunHighWatermark = static_cast<float>(*v);
            }
        }
        if (auto v = tbl["world"]["overrun_low_watermark"].value<double>()) {
            if (*v < 0.0 || *v >= cfg.overrunHighWatermark) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.overrun_low_watermark out of range [0.0, high_watermark); using default 0.60");
            } else {
                cfg.overrunLowWatermark = static_cast<float>(*v);
            }
        }
        if (auto v = tbl["world"]["overrun_min_snapshot_hz"].value<double>()) {
            if (*v < 1.0 || *v > 60.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.overrun_min_snapshot_hz out of range [1, 60]; using default 15.0");
            } else {
                cfg.overrunMinSnapshotHz = static_cast<float>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["overrun_max_ai_stride"])) {
            if (*v < int64_t{1} || *v > int64_t{32}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.overrun_max_ai_stride out of range [1, 32]; using default 4");
            } else {
                cfg.overrunMaxAiStride = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["overrun_budget_floor_bytes"])) {
            if (*v < int64_t{0} || *v > int64_t{65535}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.overrun_budget_floor_bytes out of range [0, 65535]; using default 400");
            } else {
                cfg.overrunBudgetFloorBytes = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tbl["world"]["overrun_min_interest_fraction"].value<double>()) {
            if (*v < 0.1 || *v > 1.0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.overrun_min_interest_fraction out of range [0.1, 1.0]; using default 0.5");
            } else {
                cfg.overrunMinInterestFraction = static_cast<float>(*v);
            }
        }
        if (auto v = tomlInt(tbl["world"]["max_catchup_ticks"])) {
            if (*v < int64_t{1} || *v > int64_t{64}) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "world.max_catchup_ticks out of range [1, 64]; using default 8");
            } else {
                cfg.maxCatchupTicks = static_cast<int>(*v);
            }
        }

        // [ai]
        if (auto v = tbl["ai"]["difficulty"].value<std::string>()) {
            static constexpr const char* kValidAiDifficulty[] = {"cadet", "pilot", "ace"};
            if (isOneOf(v->c_str(), kValidAiDifficulty, 3)) {
                cfg.aiDifficulty = std::move(*v);
            } else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "ai.difficulty: unknown value \"%s\"; defaulting to \"pilot\"",
                              v->c_str());
                log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
            }
        }
        // [gameplay] (#626)
        if (auto v = tbl["gameplay"]["friendly_fire"].value<bool>())
            cfg.friendlyFire = *v;
        if (auto v = tbl["gameplay"]["crash_damage"].value<bool>())
            cfg.crashDamage = *v;

        if (auto v = tbl["ai"]["difficulty_floor"].value<std::string>()) {
            if (isOneOf(v->c_str(), kValidDifficulties, 4)) {
                cfg.aiDifficultyFloor = std::move(*v);
            } else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "ai.difficulty_floor: unknown value \"%s\"; defaulting to \"recruit\"",
                              v->c_str());
                log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
            }
        }

        // [discovery]
        if (auto v = tbl["discovery"]["enabled"].value<bool>())
            cfg.discoveryEnabled = *v;
        if (auto v = tomlInt(tbl["discovery"]["interval_ms"])) {
            if (*v < 100 || *v > 60000) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "discovery.interval_ms out of range [100,60000]; using default");
            } else {
                cfg.discoveryIntervalMs = static_cast<int>(*v);
            }
        }
        if (auto v = tbl["discovery"]["query_enabled"].value<bool>())
            cfg.discoveryQueryEnabled = *v;
        if (auto v = tomlInt(tbl["discovery"]["query_port"])) {
            if (*v >= 0 && *v <= 65535)
                cfg.discoveryQueryPort = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__, "discovery.query_port out of range [0,65535]; using auto");
        }

        // [security]
        if (auto v = tomlInt(tbl["security"]["connect_rate_limit_count"])) {
            if (*v < 1 || *v > 100000) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.connect_rate_limit_count out of range [1,100000]; using default");
            } else {
                cfg.connectRateLimitCount = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["connect_rate_limit_window_s"])) {
            if (*v < 1 || *v > 3600) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.connect_rate_limit_window_s out of range [1,3600]; using default");
            } else {
                cfg.connectRateLimitWindowS = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["packet_flood_multiplier"])) {
            if (*v < 1 || *v > 100) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.packet_flood_multiplier out of range [1,100]; using default");
            } else {
                cfg.packetFloodMultiplier = static_cast<int>(*v);
            }
        }
        if (auto v = tbl["security"]["banlist_path"].value<std::string>())
            cfg.banlistPath = std::move(*v);
        if (auto v = tbl["security"]["allowlist_path"].value<std::string>())
            cfg.allowlistPath = std::move(*v);
        if (auto v = tomlInt(tbl["security"]["incoming_bandwidth_bps"])) {
            if (*v < 0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.incoming_bandwidth_bps must be >= 0; using 0 (unlimited)");
            } else {
                cfg.incomingBandwidthBps = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["outgoing_bandwidth_bps"])) {
            if (*v < 0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.outgoing_bandwidth_bps must be >= 0; using 0 (unlimited)");
            } else {
                cfg.outgoingBandwidthBps = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tbl["security"]["operator_password"].value<std::string>())
            cfg.operatorPassword = std::move(*v);
        if (auto v = tomlInt(tbl["security"]["pre_handshake_rate_limit_count"])) {
            if (*v < 0 || *v > 10000) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.pre_handshake_rate_limit_count out of range [0,10000]; using default");
            } else {
                cfg.preHandshakeRateLimitCount = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["pre_handshake_window_ms"])) {
            if (*v < 100 || *v > 60000) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.pre_handshake_window_ms out of range [100,60000]; using default");
            } else {
                cfg.preHandshakeWindowMs = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["max_connections_per_ip"])) {
            if (*v < 0 || *v > 1024) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.max_connections_per_ip out of range [0,1024]; using default");
            } else {
                cfg.maxConnectionsPerIp = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["seat_request_rate_limit_per_s"])) {
            if (*v < 1 || *v > 60) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.seat_request_rate_limit_per_s out of range [1,60]; using default");
            } else {
                cfg.seatRequestRateLimitPerS = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["team_switch_cooldown_s"])) {
            if (*v < 0 || *v > 3600) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.team_switch_cooldown_s out of range [0,3600]; using default");
            } else {
                cfg.teamSwitchCooldownS = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["heartbeat_rate_limit_per_s"])) {
            if (*v < 1 || *v > 60) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.heartbeat_rate_limit_per_s out of range [1,60]; using default");
            } else {
                cfg.heartbeatRateLimitPerS = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["admin_auth_max_failures"])) {
            if (*v < 1 || *v > 100) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.admin_auth_max_failures out of range [1,100]; using default");
            } else {
                cfg.adminAuthMaxFailures = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["admin_auth_lockout_s"])) {
            if (*v < 1 || *v > 86400) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.admin_auth_lockout_s out of range [1,86400]; using default");
            } else {
                cfg.adminAuthLockoutSeconds = static_cast<int>(*v);
            }
        }
        if (auto v = tomlInt(tbl["security"]["idle_timeout_s"])) {
            if (*v < 0 || *v > 86400) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.idle_timeout_s out of range [0,86400]; using 0 (disabled)");
            } else {
                cfg.idleTimeoutS = static_cast<int>(*v);
            }
        }

        // [shutdown]
        if (auto v = tomlInt(tbl["shutdown"]["warning_interval_s"])) {
            if (*v < 1 || *v > 86400)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "shutdown.warning_interval_s out of range [1,86400]; using default");
            else
                cfg.shutdownWarningIntervalS = static_cast<int>(*v);
        }
        if (auto v = tomlInt(tbl["shutdown"]["min_shutdown_delay_s"])) {
            if (*v < 0 || *v > 86400)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "shutdown.min_shutdown_delay_s out of range [0,86400]; using default");
            else
                cfg.minShutdownDelayS = static_cast<int>(*v);
        }
        if (auto v = tbl["shutdown"]["require_confirm"].value<bool>())
            cfg.shutdownRequireConfirm = *v;

        // [rcon]
        if (auto v = tbl["rcon"]["enabled"].value<bool>())
            cfg.rcon.enabled = *v;
        if (auto v = tomlInt(tbl["rcon"]["port"])) {
            if (*v < 1 || *v > 65535)
                log->log(LogLevel::Warn, __FILE__, __LINE__, "rcon.port out of range [1,65535]; using default");
            else
                cfg.rcon.port = static_cast<uint16_t>(*v);
        }
        if (auto v = tbl["rcon"]["password"].value<std::string>())
            cfg.rcon.password = std::move(*v);
        if (auto v = tomlInt(tbl["rcon"]["max_auth_failures"])) {
            if (*v < 1 || *v > 1000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "rcon.max_auth_failures out of range [1,1000]; using default");
            else
                cfg.rcon.maxAuthFailures = static_cast<int>(*v);
        }
        if (auto v = tomlInt(tbl["rcon"]["lockout_seconds"])) {
            if (*v < 1 || *v > 86400)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "rcon.lockout_seconds out of range [1,86400]; using default");
            else
                cfg.rcon.lockoutSeconds = static_cast<int>(*v);
        }
        if (cfg.rcon.enabled && cfg.rcon.password.empty())
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "rcon.password is empty; RCON will accept unauthenticated connections"
                     " -- set a password or disable rcon.enabled");

        // [http_admin] (#233)
        if (auto v = tbl["http_admin"]["enabled"].value<bool>())
            cfg.httpAdmin.enabled = *v;
        if (auto v = tomlInt(tbl["http_admin"]["port"])) {
            if (*v < 1 || *v > 65535)
                log->log(LogLevel::Warn, __FILE__, __LINE__, "http_admin.port out of range [1,65535]; using default");
            else
                cfg.httpAdmin.port = static_cast<uint16_t>(*v);
        }
        if (auto v = tbl["http_admin"]["bind_address"].value<std::string>())
            cfg.httpAdmin.bindAddress = std::move(*v);
        if (auto v = tomlInt(tbl["http_admin"]["max_auth_failures"])) {
            if (*v < 1 || *v > 1000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "http_admin.max_auth_failures out of range [1,1000]; using default");
            else
                cfg.httpAdmin.maxAuthFailures = static_cast<int>(*v);
        }
        if (auto v = tomlInt(tbl["http_admin"]["lockout_seconds"])) {
            if (*v < 1 || *v > 86400)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "http_admin.lockout_seconds out of range [1,86400]; using default");
            else
                cfg.httpAdmin.lockoutSeconds = static_cast<int>(*v);
        }
        if (auto* toks = tbl["http_admin"]["tokens"].as_array()) {
            for (auto& node : *toks) {
                auto* t = node.as_table();
                if (!t) {
                    log->log(LogLevel::Warn, __FILE__, __LINE__, "http_admin.tokens entries must be tables; skipping");
                    continue;
                }
                ServerConfig::HttpAdminToken row;
                if (auto v = (*t)["token"].value<std::string>())
                    row.token = std::move(*v);
                if (auto v = (*t)["role"].value<std::string>())
                    row.role = std::move(*v);
                if (auto v = tomlInt((*t)["faction"]))
                    row.faction = static_cast<int>(*v);
                if (auto v = (*t)["autonomy"].value<std::string>())
                    row.autonomy = std::move(*v);
                // A token row that names no secret authenticates nobody; dropping it here means the
                // server cannot start up believing it has an admin token that can never be presented.
                if (row.token.empty()) {
                    log->log(LogLevel::Warn, __FILE__, __LINE__,
                             "http_admin.tokens entry has an empty token; skipping");
                    continue;
                }
                cfg.httpAdmin.tokens.push_back(std::move(row));
            }
        }
        // Enabled with no usable token would be an OPEN admin API. Refuse to enable rather than warn:
        // a warning in a startup log is not a control, and this endpoint can kick, ban and shut down.
        if (cfg.httpAdmin.enabled && cfg.httpAdmin.tokens.empty()) {
            log->log(LogLevel::Error, __FILE__, __LINE__,
                     "http_admin.enabled is true but no [[http_admin.tokens]] are configured; "
                     "refusing to start an unauthenticated admin API -- add a token or disable it");
            cfg.httpAdmin.enabled = false;
        }

        // [ai.mcp] (#601) — the MCP frontend on the [http_admin] listener.
        if (auto v = tbl["ai"]["mcp"]["enabled"].value<bool>())
            cfg.mcp.enabled = *v;
        if (auto v = tbl["ai"]["mcp"]["path"].value<std::string>()) {
            // A path that is not rooted would make cpp-httplib's route never match, and the server
            // would come up reporting MCP as enabled while answering nothing.
            if (v->empty() || v->front() != '/')
                log->log(LogLevel::Warn, __FILE__, __LINE__, "ai.mcp.path must start with '/'; using default");
            else
                cfg.mcp.path = std::move(*v);
        }
        if (auto v = tbl["ai"]["mcp"]["autonomy"].value<std::string>())
            cfg.mcp.autonomy = std::move(*v);
        if (auto* list = tbl["ai"]["mcp"]["allowlist"].as_array()) {
            for (auto& node : *list)
                if (auto s = node.value<std::string>(); s && !s->empty())
                    cfg.mcp.allowlist.push_back(std::move(*s));
        }
        if (auto v = tomlInt(tbl["ai"]["mcp"]["rate_limit_per_min"])) {
            if (*v < 0 || *v > 100000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "ai.mcp.rate_limit_per_min out of range [0,100000]; using default");
            else
                cfg.mcp.rateLimitPerMin = static_cast<int>(*v);
        }
        if (auto v = tomlInt(tbl["ai"]["mcp"]["max_sessions"])) {
            if (*v < 1 || *v > 4096)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "ai.mcp.max_sessions out of range [1,4096]; using default");
            else
                cfg.mcp.maxSessions = static_cast<int>(*v);
        }
        // MCP has no listener of its own by design (plan #1036 D2): it is a second frontend on the
        // [http_admin] one, sharing its token table and per-IP lockout. Enabling it alone would be a
        // config that reads as "MCP is on" and serves nothing, so say why rather than failing quietly.
        if (cfg.mcp.enabled && !cfg.httpAdmin.enabled) {
            log->log(LogLevel::Error, __FILE__, __LINE__,
                     "ai.mcp.enabled is true but [http_admin] is disabled; MCP shares that listener and "
                     "its token table -- enable http_admin (with tokens) or disable ai.mcp");
            cfg.mcp.enabled = false;
        }
        // An act-tier default with nothing allowlisted can run nothing, which is safe but is almost
        // certainly not what the operator meant to configure.
        if (cfg.mcp.enabled && cfg.mcp.allowlist.empty() && cfg.mcp.autonomy == "act")
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "ai.mcp.autonomy is 'act' but ai.mcp.allowlist is empty; no command can be run");

        // [ai.provider] (#163) — namespaced under [ai] beside [ai.mcp], per docs/developer/ai-architecture.md §2.
        // (#163's body predates that namespacing and says [ai_provider]; the design doc and the
        // already-shipped [ai.mcp] win, so the two AI sections read as siblings.)
        if (auto v = tbl["ai"]["provider"]["enabled"].value<bool>())
            cfg.aiProvider.enabled = *v;
        if (auto v = tbl["ai"]["provider"]["plugin"].value<std::string>())
            cfg.aiProvider.plugin = std::move(*v);
        if (auto v = tbl["ai"]["provider"]["endpoint"].value<std::string>())
            cfg.aiProvider.endpoint = std::move(*v);
        if (auto v = tbl["ai"]["provider"]["model"].value<std::string>())
            cfg.aiProvider.model = std::move(*v);
        if (auto v = tbl["ai"]["provider"]["api_key_env"].value<std::string>()) {
            if (v->empty())
                log->log(LogLevel::Warn, __FILE__, __LINE__, "ai.provider.api_key_env is empty; using default");
            else
                cfg.aiProvider.apiKeyEnv = std::move(*v);
        }
        // A literal key in the config file is a mistake worth naming, not one to accept quietly:
        // server.toml gets committed and shared, and the operator almost certainly meant the env var.
        if (tbl["ai"]["provider"]["api_key"]) {
            log->log(LogLevel::Error, __FILE__, __LINE__,
                     "ai.provider.api_key is not a supported key -- put the secret in the environment "
                     "variable named by ai.provider.api_key_env (default FL_AI_API_KEY) instead");
        }
        if (auto v = tomlInt(tbl["ai"]["provider"]["max_calls_per_minute"])) {
            if (*v < 0 || *v > 100000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "ai.provider.max_calls_per_minute out of range [0,100000]; using default");
            else
                cfg.aiProvider.maxCallsPerMinute = static_cast<int>(*v);
        }
        if (auto v = tomlInt(tbl["ai"]["provider"]["world_evolution_interval_min"])) {
            if (*v < 1 || *v > 100000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "ai.provider.world_evolution_interval_min out of range [1,100000]; using default");
            else
                cfg.aiProvider.worldEvolutionIntervalMin = static_cast<int>(*v);
        }

        // [ai.chat_intent] (#611)
        if (auto v = tbl["ai"]["chat_intent"]["enabled"].value<bool>())
            cfg.chatIntent.enabled = *v;
        if (auto v = tomlInt(tbl["ai"]["chat_intent"]["rate_limit_per_min"])) {
            if (*v < 0 || *v > 10000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "ai.chat_intent.rate_limit_per_min out of range [0,10000]; using default");
            else
                cfg.chatIntent.rateLimitPerMin = static_cast<int>(*v);
        }
        if (auto v = tbl["ai"]["chat_intent"]["notify_on_decline"].value<bool>())
            cfg.chatIntent.notifyOnDecline = *v;
        // The intent tier is a CONSUMER of the provider seam. Enabled without one it would map
        // nothing and look broken, so say why rather than leaving a player wondering why their
        // wingman ignores them.
        if (cfg.chatIntent.enabled && !cfg.aiProvider.enabled) {
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "ai.chat_intent.enabled is true but [ai.provider] is disabled; free-text wingman "
                     "commands need a provider -- the radio menu remains the path");
        }

        // [metrics]
        if (auto v = tbl["metrics"]["tick_json_path"].value<std::string>())
            cfg.metrics.tickJsonPath = std::move(*v);
        if (auto v = tomlInt(tbl["metrics"]["tick_json_interval_ms"])) {
            if (*v < 100 || *v > 60000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "metrics.tick_json_interval_ms out of range [100,60000]; using default");
            else
                cfg.metrics.tickJsonIntervalMs = static_cast<uint32_t>(*v);
        }

        // [wind] — altitude wind profile (#489)
        if (auto v = tbl["wind"]["profile_path"].value<std::string>())
            cfg.wind.profilePath = std::move(*v);

        // [trace]
        if (auto v = tbl["trace"]["input_trace_dir"].value<std::string>())
            cfg.trace.inputTraceDir = std::move(*v);

        // [replay] — server-side match recording (#643)
        if (auto v = tbl["replay"]["enabled"].value<bool>())
            cfg.replay.enabled = *v;
        if (auto v = tbl["replay"]["dir"].value<std::string>())
            cfg.replay.dir = std::move(*v);
        if (auto v = tomlInt(tbl["replay"]["keyframe_interval_ticks"])) {
            if (*v < 15 || *v > 3600)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "replay.keyframe_interval_ticks out of range [15, 3600]; using default 120");
            else
                cfg.replay.keyframeIntervalTicks = static_cast<uint32_t>(*v);
        }
        if (auto v = tomlInt(tbl["replay"]["max_file_mb"])) {
            if (*v < 1 || *v > 65535)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "replay.max_file_mb out of range [1, 65535]; using default 256");
            else
                cfg.replay.maxFileMb = static_cast<uint32_t>(*v);
        }
        if (auto v = tomlInt(tbl["replay"]["max_files"])) {
            if (*v < 1 || *v > 10000)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "replay.max_files out of range [1, 10000]; using default 20");
            else
                cfg.replay.maxFiles = static_cast<uint32_t>(*v);
        }
        if (auto v = tbl["replay"]["hash_log"].value<std::string>())
            cfg.replay.hashLog = std::move(*v);

        // [network]
        if (auto v = tbl["network"]["transport"].value<std::string>()) {
            if (*v == "gns" || *v == "enet")
                cfg.network.transport = std::move(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "network.transport must be \"gns\" or \"enet\"; using default \"gns\"");
        }
        if (auto v = tbl["network"]["allow_insecure"].value<bool>())
            cfg.network.allowInsecure = *v;
        if (auto v = tbl["network"]["compress_snapshots"].value<bool>())
            cfg.network.compressSnapshots = *v;
        if (auto v = tomlInt(tbl["network"]["gns_nagle_time_us"])) {
            if (*v >= 0 && *v <= 200000)
                cfg.network.gnsNagleTimeUs = static_cast<uint32_t>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "network.gns_nagle_time_us out of range [0, 200000]; using default 0");
        }

        // [spawn]
        if (auto v = tbl["spawn"]["agl_offset"].value<double>()) {
            if (*v >= 0.0 && *v <= 50000.0)
                cfg.spawn.aglOffset = *v;
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "spawn.agl_offset out of range [0, 50000]; using default 500");
        }
        if (auto* arr = tbl["spawn"]["points"].as_array()) {
            for (auto& elem : *arr) {
                if (auto* t2 = elem.as_table()) {
                    auto xv = (*t2)["x"].value<double>();
                    auto zv = (*t2)["z"].value<double>();
                    if (xv && zv)
                        cfg.spawn.points.push_back({*xv, *zv});
                    else
                        log->log(LogLevel::Warn, __FILE__, __LINE__, "spawn.points entry missing x or z; skipping");
                }
            }
        }

        // [flight]  — the player's auto-spawned flight (#610)
        if (auto v = tomlInt(tbl["flight"]["size"])) {
            if (*v >= 0 && *v <= 8)
                cfg.flight.size = static_cast<uint32_t>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__, "flight.size out of range [0, 8]; using default 0");
        }
        if (auto v = tbl["flight"]["entity_type"].value<std::string>()) {
            if (!v->empty())
                cfg.flight.entityType = *v;
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__, "flight.entity_type empty; using default");
        }
        clampDouble(tbl, "flight", "lateral_m", 10.0, 5000.0, cfg.flight.lateralM, log);
        clampDouble(tbl, "flight", "aft_m", 0.0, 5000.0, cfg.flight.aftM, log);
        clampDouble(tbl, "flight", "vertical_m", -1000.0, 1000.0, cfg.flight.verticalM, log);
        clampDouble(tbl, "flight", "engage_range_m", 500.0, 200000.0, cfg.flight.engageRangeM, log);
        clampDouble(tbl, "flight", "cover_range_m", 500.0, 200000.0, cfg.flight.coverRangeM, log);
        clampDouble(tbl, "flight", "designate_range_m", 500.0, 200000.0, cfg.flight.designateRangeM, log);
        clampDouble(tbl, "flight", "designate_half_angle_deg", 1.0, 90.0, cfg.flight.designateHalfAngleDeg, log);
        if (auto v = tomlInt(tbl["flight"]["command_rate_limit_per_s"])) {
            if (*v >= 1 && *v <= 60)
                cfg.flight.commandRateLimitPerS = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "flight.command_rate_limit_per_s out of range [1, 60]; using default 4");
        }

        // [atc]  — air-traffic control (#706)
        if (auto v = tbl["atc"]["enabled"].value<bool>())
            cfg.atc.enabled = *v;
        if (auto v = tbl["atc"]["scramble_entity_type"].value<std::string>())
            cfg.atc.scrambleEntityType = *v;

        // [chat]  — in-match text chat (#646)
        if (auto v = tbl["chat"]["enabled"].value<bool>())
            cfg.chat.enabled = *v;
        if (auto v = tomlInt(tbl["chat"]["rate_limit_per_s"])) {
            if (*v >= 1 && *v <= 60)
                cfg.chat.rateLimitPerS = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "chat.rate_limit_per_s out of range [1, 60]; using default 2");
        }

        // [voice]  — in-game voice comms (Epic J, #532)
        if (auto v = tbl["voice"]["enabled"].value<bool>())
            cfg.voice.enabled = *v;
        if (auto v = tomlInt(tbl["voice"]["frame_rate_limit"])) {
            if (*v >= 1 && *v <= 200)
                cfg.voice.frameRateLimit = static_cast<int>(*v);
            else
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "voice.frame_rate_limit out of range [1, 200]; using default 60");
        }
        if (auto* arr = tbl["voice"]["nets"].as_array()) {
            for (auto& node : *arr) {
                const auto* nt = node.as_table();
                if (!nt)
                    continue;
                ServerConfig::VoiceNetConfig n;
                n.id = (*nt)["id"].value_or(std::string{});
                if (n.id.empty()) {
                    log->log(LogLevel::Warn, __FILE__, __LINE__, "voice.nets entry with no id; skipped");
                    continue;
                }
                n.name = (*nt)["name"].value_or(std::string{});
                n.kind = (*nt)["kind"].value_or(std::string{"team"});
                n.positional = (*nt)["positional"].value_or(false);
                n.rangeM = (*nt)["range_m"].value_or(0.0);
                n.radioEffect = (*nt)["radio_effect"].value_or(true);
                n.gain = (*nt)["gain"].value_or(1.0);
                n.defaultNet = (*nt)["default"].value_or(false);
                cfg.voice.nets.push_back(std::move(n));
            }
        }

    } catch (const toml::parse_error& e) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "failed to parse config: %s -- using defaults", e.what());
        log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        return ServerConfig{};
    }
    return cfg;
}

std::vector<WindProfileKnotDef> parseWindProfile(std::string_view content, ILogger* log) {
    std::vector<WindProfileKnotDef> knots;
    toml::table tbl;
    try {
        tbl = toml::parse(content);
    } catch (const toml::parse_error&) {
        if (log)
            log->log(LogLevel::Warn, __FILE__, __LINE__, "wind profile parse error; ignoring");
        return knots;
    }
    auto* arr = tbl["wind"]["profile"].as_array();
    if (!arr)
        return knots;
    for (auto& elem : *arr) {
        auto* t2 = elem.as_table();
        if (!t2)
            continue;
        auto alt = (*t2)["altitude_m"].value<double>();
        auto spd = (*t2)["speed_ms"].value<double>();
        auto hdg = (*t2)["heading_deg"].value<double>();
        if (!alt || !spd || !hdg) {
            if (log)
                log->log(LogLevel::Warn, __FILE__, __LINE__, "wind.profile knot missing a field; skipping");
            continue;
        }
        if (*alt < -500.0 || *alt > 40000.0 || *spd < 0.0 || *spd > 150.0 || *hdg < 0.0 || *hdg >= 360.0) {
            if (log)
                log->log(LogLevel::Warn, __FILE__, __LINE__, "wind.profile knot out of range; skipping");
            continue;
        }
        knots.push_back({static_cast<float>(*alt), static_cast<float>(*spd), static_cast<float>(*hdg)});
    }
    return knots;
}

} // namespace fl