// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ILogger.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct ServerConfig {
    // [server]
    std::string name = "Unnamed Server";
    uint16_t port = 4778;
    std::string bindAddress = "0.0.0.0";
    int maxPeers = 32;
    std::vector<std::string> gameModes = {"campaign", "mission", "sandbox"};
    std::string motd;
    uint16_t motdDisplayS{0}; // seconds; 0 = use client's motd_display_s setting
    std::string password;

    // [rotation]  — Phase 2: parsed and stored; rotation logic pending
    std::string rotationOrder = "sequential";
    std::vector<std::string> rotationItems; // each "mission" or "mission@mode" (#521)
    int rotationTimeLimitMin = 0;

    // [match]  — multiplayer match framework (#497). The default game mode for rotation items that do
    // not name their own (mission@mode) and for non-rotation servers. A "builtin:" id or a pack modes/
    // asset stem; unknown ids fall back to builtin:free-flight.
    std::string matchMode = "builtin:free-flight";
    int matchEndScreenS = 10;       // seconds the Ending phase (frozen combat + scoreboard) lasts; [0, 120]
    int matchReconnectGraceS = 120; // seconds a disconnected player's team+score is held (#524); [0, 3600], 0 = off

    // [bots]  — AI bot backfill (#87). Bots are server-side AI participants, not network peers.
    int botsFill = 0;                             // desired total participants (humans + bots); 0 = disabled; [0, 128]
    int botsMax = 16;                             // cap on live bots; [0, 127]
    std::string botsEntityType;                   // empty = [world] player_entity_type
    std::string botsAiScript = "builtin:fighter"; // AI script the bots fly
    bool botsBalanceTeams = true;                 // even the bots across teams

    // [lobby]  — Phase 2: parsed and stored; lobby registration pending (issue #36)
    bool lobbyRegister = false;
    std::string lobbyUrl = "https://lobby.fighters-legacy.org";
    std::string lobbyVisibility = "public";

    // [mods]  — Phase 2: parsed and logged; ModLoader integration pending
    std::vector<std::string> modStack;
    // Content packs a connecting client is expected to have mounted (#872). Each spec is "id" or
    // "id@version" (empty version = any). requiredPackPolicy picks what happens when a client is missing
    // one: "warn" (log + notify the client, admit), "refuse" (disconnect with the missing list), or
    // "allow_placeholder" (silently serve placeholders). Restart-only.
    std::vector<std::string> requiredPacks;
    std::string requiredPackPolicy = "warn";

    // [world]  — Phase 2: active only with --persistent flag
    bool persistent = false;
    std::string worldSavePath = "world.sav";
    // Entity type spawned for a connecting pilot when the client requests none (#834). A client may
    // request a specific type in MsgConnectRequest; the server clamps it to a REGISTERED type and falls
    // back to this default, then to builtin:debug-entity. Lets "boot server, connect, look at the
    // aeroplane" be a config change instead of an engine patch.
    std::string playerEntityType = "builtin:debug-entity";
    bool allowObservers = true; // #857: false = refuse observer-role connect requests
    int worldAutosaveIntervalS = 300;
    int entitySoftCap = 0;              // 0 = unlimited; server-enforced object count limit
    double timeScale = 10.0;            // game seconds per real second; 10 = full day/night ~2.4 real hrs
    double planetRadiusM = 6'371'000.0; // sphere radius (m); Earth default
    bool earthRotation = true;          // #482: Coriolis + centrifugal in the Earth-fixed world frame
    // Per-peer interest radius (km); [1, 100000]. 100 km (#1093, D19): AoI is PRESENTATION
    // relevance, not sensor truth — sensor knowledge reaches players through the datalink, not the
    // snapshot radius — and 128 fighters are never 200 km apart, so the old default culled almost
    // nothing while a full-radius query still visited a 41x41 cell box per peer per tick.
    double drawDistanceKm = 100.0;
    int spectateDelayS = 0; // dead/observer snapshot delay (s); anti-ghosting; [0, 300]; 0 = off (#403)
    // SpatialIndex cell size (km); 0 = AUTO from the draw distance; [0, 1000]; restart. Auto is the
    // default since #1093: the mechanism has existed since #573 (clamp(drawDist/32, 500 m, 10 km)),
    // it simply was not what shipped, so a fixed 10 km cell sized the query box for a radius nobody
    // was using.
    double spatialCellSizeKm = 0.0;
    uint32_t snapshotBudgetBytes = 1200; // per-client snapshot byte budget; 0 = unlimited; [0, 65535] (#516)
    uint32_t jitterBufferDepth = 4;      // per-peer input queue depth (ticks); [1, 32]
    uint32_t jitterAdaptWindow = 60;     // EWMA smoothing window in ticks; [10, 3600]
    uint32_t jitterHysteresis = 2;       // resize dead-band in ticks; [0, 8]
    float jitterMultiplier = 2.0f;       // k factor: depth = ceil(ewma + k*jitter); [0.0, 8.0]
    // Adaptive per-client send-rate / congestion response (#518). Hot-reloadable via reload_config.
    bool congestionEnabled = true;             // false = always full 60 Hz / full budget
    float congestionMinSendHz = 10.0f;         // floor send rate under congestion; [1, 60]
    float congestionLossThreshold = 0.02f;     // ENet mean loss fraction => congested; [0, 1]
    uint32_t congestionBudgetFloorBytes = 400; // never scale a set byte budget below this; [0, 65535]
    // Graceful tick-overrun governor (#514). Sheds snapshot/AI work when the tick exceeds budget under
    // load. Hot-reloadable via reload_config, except maxCatchupTicks (a GameLoop ctor value).
    bool overrunGovernorEnabled = true;      // false = no degradation (loadFactor pinned to 1)
    float overrunHighWatermark = 0.90f;      // EWMA tick-ms / budget that triggers shedding; [0.1, 1.0]
    float overrunLowWatermark = 0.60f;       // recovery threshold (dead-band below high); [0.0, high)
    float overrunMinSnapshotHz = 15.0f;      // floor broadcast rate under overrun; [1, 60]
    uint32_t overrunMaxAiStride = 4;         // deepest AI-sample decimation; [1, 32]
    uint32_t overrunBudgetFloorBytes = 400;  // never scale the snapshot budget below this; [0, 65535]
    float overrunMinInterestFraction = 0.5f; // interest-radius floor fraction; [0.1, 1.0]; 1 = lever off (#726)
    int maxCatchupTicks = 8;                 // GameLoop catch-up cap (spiral backstop); [1, 64]; restart
    // Sim-tick CPU parallelism: total worker threads for the per-entity AI + integrate passes,
    // including the sim thread. 0 = auto (hardware_concurrency), 1 = serial. CPU knob, NOT a
    // capacity guarantee. CLI --sim-worker-threads overrides this. [0, 256]
    // Sensor geometry checks per second (#685). 10 is the REFERENCE cadence every authored `pod` is
    // tuned against — changing it changes effective acquisition time, which is the honest
    // consequence and is documented rather than silently renormalized. [1, 60]
    double sensorCheckHz = 10.0;

    uint32_t simWorkerThreads = 0;
    // Load-test affordance (#573): spawn N server-side AI entities (cheap loiter controllers spread over
    // testSpawnSpreadKm at testSpawnAglM) at startup to stress the entity pool + SpatialIndex at scale.
    // A TESTING AFFORDANCE, NOT A CAPACITY GUARANTEE. 0 = disabled. Restart-only.
    uint32_t testSpawnAiCount = 0;   // number of AI entities to pre-spawn; [0, 1000000]
    double testSpawnSpreadKm = 50.0; // spread radius (km) of the phyllotaxis distribution; [0, 100000]
    double testSpawnAglM = 500.0;    // spawn/loiter altitude above the entity's OWN local terrain (m); [0, 50000]
    // #580: weighted controller mix for the pre-spawned entities, "loiter:70,pursuit:20,patrol:10"
    // (deterministic per-index assignment; behaviors: loiter | pursuit | patrol). Empty = all loiter
    // (the #573 baseline). Invalid specs log Warn and fall back to all loiter. Restart-only.
    std::string testSpawnAiMix;
    // #966/#980: the entity type the load AI spawns. Default builtin:debug-entity (the #573 baseline);
    // set to builtin:bomber to run CREWED AI aircraft (a bot tail-gunner per airframe), exercising the
    // per-seat sample/slew/fire passes + turret pose replication under 128-client scale-gate load. An
    // unregistered type stops the spawn cleanly. Restart-only.
    std::string testSpawnEntityType;
    // #580: projectile-churn generator — spawn testProjectileRate short-lived entities per second,
    // each killed after testProjectileTtlS, to stress the pool free-list, O(liveCount) forEach under
    // fragmentation, and the SnapshotDespawn TLV path. 0 = disabled. Restart-only.
    double testProjectileRate = 0.0; // spawns per second; [0, 100000]
    double testProjectileTtlS = 3.0; // lifetime of each churned entity (s); [0.05, 600]

    // Faction stamped onto every player entity on connect (#610). MUST be non-zero for any threat
    // logic to work: fl::areFactionsHostile treats faction 0 as neutral — an entity with NO ENEMIES.
    // With the legacy value of 0 nothing in the world is hostile to a player, so a wingman's
    // engage/cover conditions can never fire and boresight designation can never designate.
    // Setting 0 restores the pre-#610 behavior exactly (and disables the wingman's threat logic —
    // fl-server warns at startup if a flight is configured alongside it). [0, 65535]
    uint16_t playerFaction = 1;

    // [ai]
    // What the SERVER runs (#682): resolved to AiScaling and fed to the sim tick, where it scales
    // radar range (radarSensorRange) and the AI's reaction delay (reactionTimeS). `cadet|pilot|ace`;
    // unknown value logs Warn and keeps the default. Hot-reloadable via reload_config.
    std::string aiDifficulty = "pilot";

    // A FUTURE PER-CLIENT CLAMP, not what the server runs — distinct from `difficulty` above.
    // Phase 2: parsed and stored; enforcement lands with the AI runtime.
    std::string aiDifficultyFloor = "recruit";

    // [gameplay] — the damage gates (#626). SERVER-authoritative: the client difficulty screen's
    // matching toggles configure single-player through this same path (the embedded fl-server),
    // never the client directly. Hot-reloadable via reload_config.
    bool friendlyFire = false; // same-faction weapon damage suppressed when false
    bool crashDamage = true;   // ground impacts damage the airframe when true

    // [discovery]
    bool discoveryEnabled = true;
    int discoveryIntervalMs = 2000;
    bool discoveryQueryEnabled = true; // #997: answer server-info queries
    int discoveryQueryPort = 0;        // 0 = auto (game port + 1); [0, 65535]

    // [shutdown]
    int shutdownWarningIntervalS = 300; // seconds between countdown broadcast notices (default 5 min)
    int minShutdownDelayS = 0;          // minimum seconds of warning required; 0 = no minimum
    bool shutdownRequireConfirm = true; // require --force flag before scheduling shutdown

    // [security]
    int connectRateLimitCount = 5;     // max connections per IP within the window
    int connectRateLimitWindowS = 10;  // sliding window size, seconds
    int packetFloodMultiplier = 3;     // disconnect if peer sends > N * 60 MsgClientInput/s
    std::string banlistPath;           // one normalized IP per line; empty = no persistence
    std::string allowlistPath;         // allowlist file; empty = disabled (all IPs allowed)
    uint32_t incomingBandwidthBps = 0; // ENet host incoming cap, bytes/s; 0 = unlimited
    uint32_t outgoingBandwidthBps = 0; // ENet host outgoing cap, bytes/s; 0 = unlimited
    std::string operatorPassword; // empty = network admin commands disabled; overridden by --admin-token at runtime
    int preHandshakeRateLimitCount = 20; // max CONNECT attempts per IP per window; 0 = disabled
    int preHandshakeWindowMs = 1000;     // sliding window in milliseconds
    int maxConnectionsPerIp = 0;         // max simultaneous connections per IP; 0 = unlimited
    int adminAuthMaxFailures = 5;        // consecutive wrong-password attempts before per-IP lockout [1,100]
    int adminAuthLockoutSeconds = 300;   // per-IP lockout duration in seconds [1,86400]
    int idleTimeoutS = 0;                // disconnect peers with no activity for N seconds; 0 = disabled [0,86400]
    // World-mutating request limits (#1069). A seat or team grant despawns and respawns an entity and
    // re-sends the whole ConnectAck type table; a heartbeat draws a MsgPeerDelay reply.
    int seatRequestRateLimitPerS = 2; // seat requests per second per peer [1,60]
    int teamSwitchCooldownS = 5;      // seconds between accepted team switches per peer; 0 = none [0,3600]
    int heartbeatRateLimitPerS = 4;   // heartbeats per second per peer that draw a reply [1,60]

    // [rcon]
    struct RconConfig {
        bool enabled = false;
        uint16_t port = 27015;
        std::string password;    // empty + enabled = warn at startup; unauthenticated connections accepted
        int maxAuthFailures = 5; // lock out IP after this many consecutive failures
        int lockoutSeconds = 60; // per-IP lockout duration in seconds
    };
    RconConfig rcon;

    // [http_admin] — the REST admin API + health endpoint (#233). Disabled by default, and bound to
    // localhost when enabled: an admin surface that appears on 0.0.0.0 the moment an operator flips
    // one boolean is a footgun, so exposing it takes a second, deliberate edit.
    //
    // Tokens are DATA, not roles hardcoded in C++ (the [[voice.nets]] precedent). Each row maps a
    // bearer token onto a role preset from the #945 capability vocabulary, so the REST frontend
    // resolves a request to a CommandIssuer and calls the same permission-checked dispatch the ENet
    // admin channel does -- there is no second permission system to keep in sync.
    struct HttpAdminToken {
        std::string token;
        std::string role = "admin"; // a parseRolePreset name: admin|moderator|gm|faction_leader
        int faction = -1;           // faction index for a faction-scoped role; -1 = unbound
        // Per-token MCP autonomy tier override (#601): observe|recommend|act. Empty = inherit
        // [ai.mcp] autonomy. One token table serves REST and MCP (plan #1036 D3) — the tier is an
        // ADDITIONAL gate in front of the role's capability mask, never a replacement for it.
        std::string autonomy;
    };
    struct HttpAdminConfig {
        bool enabled = false;
        uint16_t port = 8080;
        std::string bindAddress = "127.0.0.1";
        std::vector<HttpAdminToken> tokens;
        int maxAuthFailures = 5; // per-IP lockout threshold, the RCON/admin-channel policy
        int lockoutSeconds = 300;
    };
    HttpAdminConfig httpAdmin;

    // [ai.mcp] — the Model Context Protocol surface (#601). A SECOND FRONTEND on the [http_admin]
    // listener, not a second server: it shares that listener, that token table and that per-IP
    // lockout, so enabling MCP requires [http_admin] to be enabled too.
    //
    // Distinct from the [ai] difficulty section above, which is what the sim runs.
    struct McpConfig {
        bool enabled = false;
        std::string path = "/mcp"; // Streamable HTTP endpoint: POST for calls, GET for notifications
        // Default autonomy tier for a token whose row does not override it. `observe` — read-only —
        // because a default that could act would make forgetting a field into granting authority.
        std::string autonomy = "observe";
        // Commands an `act`-tier token may run. EMPTY PERMITS NOTHING: enabling MCP without listing
        // commands does not implicitly authorize all of them.
        std::vector<std::string> allowlist;
        int rateLimitPerMin = 120; // per token; <= 0 disables the limiter
        int maxSessions = 32;      // concurrent MCP sessions; oldest idle is evicted beyond this
    };
    McpConfig mcp;

    // [ai.provider] — the generative-AI provider seam (#163). OPT-IN: an operator must set
    // `enabled = true` before the server will talk to any model at all.
    //
    // Every AI feature degrades to its scripted path when this is off, and that fallback is the
    // CI-tested one (docs/developer/ai-architecture.md §7).
    struct AiProviderConfig {
        bool enabled = false;
        std::string plugin;   // path to an IWorldAiProvider shared library; empty = NullAiProvider
        std::string endpoint; // backend-specific; interpreted by the plugin, not by fl-server
        std::string model;
        // The API key is NEVER a config value. `apiKeyEnv` names the ENVIRONMENT VARIABLE holding
        // it, defaulting to FL_AI_API_KEY — the same rule the ai_eval harness follows and the same
        // reason: a key in server.toml gets committed, and a key on a command line shows up in `ps`.
        std::string apiKeyEnv = "FL_AI_API_KEY";
        int maxCallsPerMinute = 10;
        int worldEvolutionIntervalMin = 60; // in-game minutes between evolution calls
    };
    AiProviderConfig aiProvider;

    // [ai.chat_intent] — free-text wingman commands over team chat (#611).
    //
    // Needs [ai.provider] with the `intent` capability. With either missing, the radio menu is the
    // path — which is the #769 decision, not a degradation to apologise for.
    struct ChatIntentConfig {
        bool enabled = false;
        // Intent requests per minute per peer. Separate from the chat rate limit and much lower: a
        // chat line is free, a model call is not, and the team channel must not become a lever
        // against the server's own inference budget.
        int rateLimitPerMin = 6;
        // Tell the pilot when a call was understood, declined, or not attempted. Off would leave a
        // player unable to tell "the wingman ignored me" from "the feature is not on".
        bool notifyOnDecline = true;
    };
    ChatIntentConfig chatIntent;

    // [metrics]
    struct MetricsConfig {
        std::string tickJsonPath;           // empty = disabled; atomic per-interval tick-budget JSON export
        uint32_t tickJsonIntervalMs = 1000; // write cadence in ms; [100, 60000]
    };
    MetricsConfig metrics;

    // [wind]  — altitude wind profile (#489)
    struct WindConfig {
        std::string profilePath; // empty = disabled; path (relative to the config dir) to a wind
                                 // profile TOML (see tools/gen_wind_profile.py). Loaded once at
                                 // startup and applied via WeatherController::setWindProfile.
    };
    WindConfig wind;

    // [trace]  — server-side input tracing (#560)
    struct TraceConfig {
        std::string inputTraceDir; // empty = disabled; per-peer FLIT traces written here
    };
    TraceConfig trace;

    // [replay]  — server-side match recording to `.flrep` (#643)
    //
    // On by default: #41's acceptance is "every mission recorded automatically", and a replay nobody
    // remembered to switch on is not a replay. The disk cost is bounded by rotation, not by trust.
    struct ReplayConfig {
        bool enabled = true;
        std::string dir = "replays"; // relative to the server's working directory
        // Ticks between keyframes -- the seek granularity a scrub lands on. Measured rather than
        // guessed (#643): across a 10x range (60 -> 600 ticks) on a 40-entity moving world the file
        // grew only 0.4 %, because a delta record of a moving entity is nearly the size of a full one
        // and zstd eats the repetition. So cadence is chosen for SEEK, not size: 120 = 2 s at 60 Hz.
        uint32_t keyframeIntervalTicks = 120;
        uint32_t maxFileMb = 256; // rotate to a new file past this size; [1, 65535]
        // Bounds the DIRECTORY, oldest first -- a server that records one file per match must not
        // fill the disk one perfectly-rotated file at a time. [1, 10000].
        uint32_t maxFiles = 20;
        // Per-tick state-hash sidecar (#644's gate reads it). Empty = not written; this is a test
        // instrument, not something a live server needs.
        std::string hashLog;
    };
    ReplayConfig replay;

    // [spawn]
    struct SpawnPointDef {
        double x = 0.0;
        double z = 0.0;
    };
    struct SpawnConfig {
        double aglOffset = 2.0;            // metres AGL above terrain for all spawn points
        std::vector<SpawnPointDef> points; // empty = use origin
    };
    SpawnConfig spawn;

    // [flight]  — the player's flight: AI wingmen auto-spawned per connecting peer (#610)
    //
    // `size` DEFAULTS TO 0 on purpose. N extra AI entities per peer would move every scale-gate and
    // load-test number, so a dedicated server is byte-for-byte unchanged unless an operator asks for
    // a flight. Single-player still works out of the box: LocalServer passes --flight-size 1, which
    // is the Phase 4 acceptance path ("wingman follows player and responds to all six commands").
    //
    // Formations larger than the auto-spawned player flight — all-AI flights, strike packages, an
    // AWACS commanding aircraft it is not flying with — are built at runtime through the `flight`
    // admin command family, not here. This section only describes what a peer gets on connect.
    struct FlightConfig {
        uint32_t size = 0;                               // AI wingmen spawned per connecting peer; 0 = disabled; [0, 8]
        std::string entityType = "builtin:debug-entity"; // entity type spawned for each member
        double lateralM = 150.0;                         // formation slot spacing: lateral, per rank; [10, 5000]
        double aftM = 100.0;                             // aft, per rank; [0, 5000]
        double verticalM = -15.0;                        // vertical, per rank (negative = stepped down); [-1000, 1000]
        double engageRangeM = 12000.0;       // engage_bandits trigger radius about the member; [500, 200000]
        double coverRangeM = 6000.0;         // cover_me trigger radius about the ANCHOR; [500, 200000]
        double designateRangeM = 15000.0;    // attack_my_target: boresight designation range; [500, 200000]
        double designateHalfAngleDeg = 15.0; // boresight cone half-angle; [1, 90]
        int commandRateLimitPerS = 4;        // wingman orders per second per peer; [1, 60]
    };
    FlightConfig flight;

    // [atc]  — air-traffic control service (#706)
    struct AtcConfig {
        bool enabled = true;                                     // build the ATC service + facilities
        std::string scrambleEntityType = "builtin:debug-entity"; // default type for atc_scramble / atc.scramble
    };
    AtcConfig atc;

    // [chat]  — in-match text chat (#646)
    struct ChatConfig {
        bool enabled = true;   // route player chat lines; false = drop all chat
        int rateLimitPerS = 2; // chat lines per second per peer; [1, 60]
    };
    ChatConfig chat;

    // [voice]  — in-game voice comms (Epic J, #532)
    //
    // Nets are DATA, not code: an operator adds a "tanker" or "awacs" net without an engine change.
    // Leave [[voice.nets]] out entirely and the compiled-in stack (team / flight / atc / proximity)
    // is used, so voice works with zero configuration.
    struct VoiceNetConfig {
        std::string id;            // stable machine id; addressed by config and admin commands
        std::string name;          // display label; empty = the id
        std::string kind = "team"; // global | team | flight | proximity | atc
        bool positional = false;   // mix at the speaker's world position instead of head-locked
        double rangeM = 0.0;       // proximity radius / positional rolloff ceiling; 0 = unlimited
        bool radioEffect = true;   // apply the radio DSP (#925)
        double gain = 1.0;         // per-net trim; [0, 4]
        bool defaultNet = false;   // pre-selected under the client's primary PTT key
        // Concurrent-speaker cap (#1090, D20); 0 = unlimited. Relay cost is (talkers x listeners),
        // and only the listener side was ever bounded.
        int maxTalkers = 4;
    };
    struct VoiceConfig {
        bool enabled = true; // false = the server relays no audio and tells clients voice is off
        // frames/s/peer; [1, 200]. A BANDWIDTH bound, not anti-spam: a frame is fanned out to every
        // recipient, so the cost is recipients x bytes. 52, not 60 (#1090): the codec produces 50/s,
        // so the old default sat ABOVE what a well-behaved client could reach and bound nothing.
        int frameRateLimit = 52;
        std::vector<VoiceNetConfig> nets; // empty = builtinRadioNets()
    };
    VoiceConfig voice;

    // [network]  — transport backend selection (#507)
    struct NetworkConfig {
        std::string transport = "gns"; // "gns" (GameNetworkingSockets, default) or "enet" (enet6)
        bool allowInsecure = true;     // GNS: accept unauthenticated peers (no Steam PKI); AllowWithoutAuth
        bool compressSnapshots = true; // zstd snapshot payload compression (#775); hot-reloadable
        uint32_t gnsNagleTimeUs = 0;   // GNS datagram-coalescing window, us; 0 = GNS default (5000);
                                       // range [0, 200000]; restart-only (#775)
    };
    NetworkConfig network;
};

// Returns the embedded default server.toml content written on first run.
std::string_view defaultServerConfigToml();

// A wind-profile knot as authored (#489): altitude (m MSL), speed (m/s), heading the wind blows FROM
// (deg). Mirrors WeatherController::WindProfileMetKnot without coupling this header to engine-weather.
struct WindProfileKnotDef {
    float altM = 0.0f;
    float speedMs = 0.0f;
    float headingDeg = 0.0f;
};

// Parse a wind-profile TOML ([[wind.profile]] entries; see tools/gen_wind_profile.py) into knots.
// Malformed/out-of-range knots are skipped with a Warn; returns the valid knots (possibly empty).
std::vector<WindProfileKnotDef> parseWindProfile(std::string_view content, ILogger* log);

// Parse server configuration from a TOML string.
// On parse error, logs a Warn and returns a default-constructed ServerConfig.
ServerConfig parseServerConfig(std::string_view content, ILogger* log);

} // namespace fl
