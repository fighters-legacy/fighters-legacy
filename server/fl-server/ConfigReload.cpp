// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConfigReload.h"

#include "ServerCommands.h"

#include <PostgresStore.h> // redactDsn — the reload report must not echo a connection string

#include <console/CommandShell.h>
#include <entity/EntityManager.h>
#include <loop/GameLoop.h>
#include <net/DiscoveryBeacon.h>
#include <net/WorldBroadcaster.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace fl {
namespace {

// ---------------------------------------------------------------------------
// Value readers
// ---------------------------------------------------------------------------

std::string valueText(const std::string& v) {
    return v;
}
std::string valueText(bool v) {
    return v ? "true" : "false";
}
std::string valueText(int v) {
    return std::to_string(v);
}
std::string valueText(unsigned int v) {
    return std::to_string(v);
}
std::string valueText(uint16_t v) {
    return std::to_string(static_cast<unsigned>(v));
}
// Trailing zeros trimmed: an operator comparing `100` with `100.000000` should see no difference,
// because there is none.
std::string valueText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}
std::string valueText(float v) {
    return valueText(static_cast<double>(v));
}
// An array key is one value to an operator: they edited the list or they did not.
std::string valueText(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i)
            out += ", ";
        out += v[i];
    }
    out += "]";
    return out;
}

// A credential never appears in a diff: reload_config's output goes to stdout, the shell ring, the
// RCON socket and the /events mirror at once. Whether one is SET is the operator-useful fact.
std::string secretText(const std::string& v) {
    return v.empty() ? "<unset>" : "<set>";
}

// ---------------------------------------------------------------------------
// Hot-key appliers
//
// Each runs on the sim thread inside reload_config's single enqueued callback. A row reads its own
// key from `incoming` and anything RESTART-ONLY from `running` -- see ReloadApplyContext.
// ---------------------------------------------------------------------------

void applyBeaconName(const ReloadApplyContext& rc) {
    if (rc.ctx.env.beacon)
        rc.ctx.env.beacon->setName(rc.incoming.server.name);
}

void applyMotd(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setMotd(rc.incoming.server.motd);
}

void applyMotdDisplay(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setMotdDisplaySeconds(rc.incoming.server.motdDisplayS);
}

void applyEntitySoftCap(const ReloadApplyContext& rc) {
    if (!rc.ctx.sim.entityManager)
        return;
    // #1049: raising the cap is how an operator relieves a world that is refusing spawns, and waiting
    // for a restart to do that is waiting through the outage. The player RESERVE comes from the
    // RUNNING max_peers, which is restart-only -- a reload must not widen it.
    const auto cap = static_cast<uint32_t>(rc.incoming.world.entitySoftCap < 0 ? 0 : rc.incoming.world.entitySoftCap);
    const auto reserve = static_cast<uint32_t>(rc.running.server.maxPeers < 0 ? 0 : rc.running.server.maxPeers);
    rc.ctx.sim.entityManager->setSoftCap(cap, reserve);
}

void applyDrawDistance(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setDrawDistance(static_cast<float>(rc.incoming.world.drawDistanceKm));
}

void applySnapshotBudget(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setSnapshotBudget(rc.incoming.world.snapshotBudgetBytes);
}

void applySnapshotCompression(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setSnapshotCompression(rc.incoming.network.compressSnapshots);
}

void applyJitterDepth(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setJitterBufferDepth(rc.incoming.world.jitterBufferDepth);
}

void applyJitterAdaptWindow(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setJitterAdaptWindow(rc.incoming.world.jitterAdaptWindow);
}

void applyJitterHysteresis(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setJitterHysteresis(rc.incoming.world.jitterHysteresis);
}

void applyJitterMultiplier(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setJitterMultiplier(rc.incoming.world.jitterMultiplier);
}

// The congestion and governor levers are parameter STRUCTS built from several keys, so each of their
// keys re-applies the whole struct from the incoming config. Idempotent, and it keeps the row-per-key
// rule intact rather than inventing a "group" concept whose membership would be another list to drift.
void applyCongestion(const ReloadApplyContext& rc) {
    const auto& w = rc.incoming.world;
    rc.ctx.sim.broadcaster->setCongestionParams(fl::makeCongestionParams(
        w.congestionEnabled, w.congestionMinSendHz, w.congestionLossThreshold, w.congestionBudgetFloorBytes));
}

void applyGovernor(const ReloadApplyContext& rc) {
    const auto& w = rc.incoming.world;
    rc.ctx.sim.broadcaster->setGovernorParams(fl::makeTickGovernorParams(
        w.overrunGovernorEnabled, w.overrunHighWatermark, w.overrunLowWatermark, w.overrunMinSnapshotHz,
        w.overrunMaxAiStride, w.overrunBudgetFloorBytes, w.overrunMinInterestFraction));
}

void applySensorCheckHz(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setSensorCheckHz(static_cast<float>(rc.incoming.world.sensorCheckHz));
}

void applyDamageRules(const ReloadApplyContext& rc) {
    rc.ctx.sim.broadcaster->setDamageRules(
        fl::DamageRules{rc.incoming.gameplay.friendlyFire, rc.incoming.gameplay.crashDamage});
}

void applyAiDifficulty(const ReloadApplyContext& rc) {
    // nullopt = no resolver wired, so leave the running scaling alone rather than reset it to a
    // default the operator did not ask for (#682).
    if (rc.aiScaling)
        rc.ctx.sim.broadcaster->setAiScaling(*rc.aiScaling);
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

constexpr std::string_view kRejected[] = {"ai.provider.api_key"};

const std::array kTable = std::to_array<ConfigKeyInfo>({
    {"server.name", ReloadClass::Hot, [](const ServerConfig& c) { return valueText(c.server.name); }, applyBeaconName},
    {"server.port", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.server.port); }, nullptr},
    {"server.bind_address", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.server.bindAddress); },
     nullptr},
    {"server.max_peers", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.server.maxPeers); },
     nullptr},
    {"server.motd", ReloadClass::Hot, [](const ServerConfig& c) { return valueText(c.server.motd); }, applyMotd},
    {"server.motd_display_s", ReloadClass::Hot, [](const ServerConfig& c) { return valueText(c.server.motdDisplayS); },
     applyMotdDisplay},
    {"server.password", ReloadClass::Restart, [](const ServerConfig& c) { return secretText(c.server.password); },
     nullptr},
    {"server.game_modes", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.server.gameModes); },
     nullptr},
    {"rotation.order", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.rotation.order); },
     nullptr},
    {"rotation.items", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.rotation.items); },
     nullptr},
    {"rotation.time_limit_min", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.rotation.timeLimitMin); }, nullptr},
    {"match.mode", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.match.mode); }, nullptr},
    {"match.end_screen_s", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.match.endScreenS); },
     nullptr},
    {"match.reconnect_grace_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.match.reconnectGraceS); }, nullptr},
    {"bots.fill", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.bots.fill); }, nullptr},
    {"bots.max_bots", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.bots.max); }, nullptr},
    {"bots.entity_type", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.bots.entityType); },
     nullptr},
    {"bots.ai_script", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.bots.aiScript); }, nullptr},
    {"bots.balance_teams", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.bots.balanceTeams); },
     nullptr},
    {"lobby.register", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.lobby.registerServer); },
     nullptr},
    {"lobby.url", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.lobby.url); }, nullptr},
    {"lobby.visibility", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.lobby.visibility); },
     nullptr},
    {"mods.stack", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.mods.stack); }, nullptr},
    {"mods.required", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.mods.requiredPacks); },
     nullptr},
    {"mods.required_policy", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.mods.requiredPackPolicy); }, nullptr},
    {"world.player_entity_type", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.playerEntityType); }, nullptr},
    {"world.allow_observers", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.allowObservers); }, nullptr},
    {"world.entity_soft_cap", ReloadClass::Hot, [](const ServerConfig& c) { return valueText(c.world.entitySoftCap); },
     applyEntitySoftCap},
    {"world.time_scale", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.world.timeScale); },
     nullptr},
    {"world.planet_radius_m", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.planetRadiusM); }, nullptr},
    {"world.earth_rotation", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.earthRotation); }, nullptr},
    {"world.draw_distance_km", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.drawDistanceKm); }, applyDrawDistance},
    {"world.spectate_delay_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.spectateDelayS); }, nullptr},
    {"world.spatial_cell_size_km", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.spatialCellSizeKm); }, nullptr},
    {"world.snapshot_budget_bytes", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.snapshotBudgetBytes); }, applySnapshotBudget},
    {"world.jitter_buffer_depth", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.jitterBufferDepth); }, applyJitterDepth},
    {"world.jitter_buffer_adapt_window", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.jitterAdaptWindow); }, applyJitterAdaptWindow},
    {"world.jitter_buffer_hysteresis", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.jitterHysteresis); }, applyJitterHysteresis},
    {"world.jitter_buffer_jitter_multiplier", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.jitterMultiplier); }, applyJitterMultiplier},
    {"world.congestion_enabled", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.congestionEnabled); }, applyCongestion},
    {"world.congestion_min_send_hz", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.congestionMinSendHz); }, applyCongestion},
    {"world.congestion_loss_threshold", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.congestionLossThreshold); }, applyCongestion},
    {"world.congestion_budget_floor_bytes", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.congestionBudgetFloorBytes); }, applyCongestion},
    {"world.overrun_governor_enabled", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.overrunGovernorEnabled); }, applyGovernor},
    {"world.overrun_high_watermark", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.overrunHighWatermark); }, applyGovernor},
    {"world.overrun_low_watermark", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.overrunLowWatermark); }, applyGovernor},
    {"world.overrun_min_snapshot_hz", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.overrunMinSnapshotHz); }, applyGovernor},
    {"world.overrun_max_ai_stride", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.overrunMaxAiStride); }, applyGovernor},
    {"world.overrun_budget_floor_bytes", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.overrunBudgetFloorBytes); }, applyGovernor},
    {"world.overrun_min_interest_fraction", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.world.overrunMinInterestFraction); }, applyGovernor},
    {"world.max_catchup_ticks", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.maxCatchupTicks); }, nullptr},
    {"world.sensor_check_hz", ReloadClass::Hot, [](const ServerConfig& c) { return valueText(c.world.sensorCheckHz); },
     applySensorCheckHz},
    {"world.sim_worker_threads", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.simWorkerThreads); }, nullptr},
    {"world.test_spawn_ai_count", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.testSpawnAiCount); }, nullptr},
    {"world.test_spawn_spread_km", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.testSpawnSpreadKm); }, nullptr},
    {"world.test_spawn_agl_m", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.testSpawnAglM); }, nullptr},
    {"world.test_spawn_ai_mix", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.testSpawnAiMix); }, nullptr},
    {"world.test_spawn_entity_type", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.testSpawnEntityType); }, nullptr},
    {"world.test_projectile_rate", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.testProjectileRate); }, nullptr},
    {"world.test_projectile_ttl_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.testProjectileTtlS); }, nullptr},
    {"world.player_faction", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.world.playerFaction); }, nullptr},
    {"ai.difficulty", ReloadClass::Hot, [](const ServerConfig& c) { return valueText(c.ai.difficulty); },
     applyAiDifficulty},
    {"ai.difficulty_floor", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.difficultyFloor); },
     nullptr},
    {"ai.mcp.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.mcp.enabled); },
     nullptr},
    {"ai.mcp.path", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.mcp.path); }, nullptr},
    {"ai.mcp.autonomy", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.mcp.autonomy); },
     nullptr},
    {"ai.mcp.rate_limit_per_min", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.mcp.rateLimitPerMin); }, nullptr},
    {"ai.mcp.max_sessions", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.mcp.maxSessions); },
     nullptr},
    {"ai.mcp.allowlist", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.mcp.allowlist); },
     nullptr},
    {"ai.provider.enabled", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.provider.enabled); }, nullptr},
    {"ai.provider.plugin", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.provider.plugin); },
     nullptr},
    {"ai.provider.endpoint", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.provider.endpoint); }, nullptr},
    {"ai.provider.model", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.ai.provider.model); },
     nullptr},
    {"ai.provider.api_key_env", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.provider.apiKeyEnv); }, nullptr},
    {"ai.provider.max_calls_per_minute", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.provider.maxCallsPerMinute); }, nullptr},
    {"ai.provider.world_evolution_interval_min", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.provider.worldEvolutionIntervalMin); }, nullptr},
    {"ai.chat_intent.enabled", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.chatIntent.enabled); }, nullptr},
    {"ai.chat_intent.rate_limit_per_min", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.chatIntent.rateLimitPerMin); }, nullptr},
    {"ai.chat_intent.notify_on_decline", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.ai.chatIntent.notifyOnDecline); }, nullptr},
    {"gameplay.friendly_fire", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.gameplay.friendlyFire); }, applyDamageRules},
    {"gameplay.crash_damage", ReloadClass::Hot, [](const ServerConfig& c) { return valueText(c.gameplay.crashDamage); },
     applyDamageRules},
    {"discovery.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.discovery.enabled); },
     nullptr},
    {"discovery.interval_ms", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.discovery.intervalMs); }, nullptr},
    {"discovery.query_enabled", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.discovery.queryEnabled); }, nullptr},
    {"discovery.query_port", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.discovery.queryPort); }, nullptr},
    {"shutdown.warning_interval_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.shutdown.warningIntervalS); }, nullptr},
    {"shutdown.min_shutdown_delay_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.shutdown.minDelayS); }, nullptr},
    {"shutdown.require_confirm", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.shutdown.requireConfirm); }, nullptr},
    {"security.connect_rate_limit_count", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.connectRateLimitCount); }, nullptr},
    {"security.connect_rate_limit_window_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.connectRateLimitWindowS); }, nullptr},
    {"security.packet_flood_multiplier", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.packetFloodMultiplier); }, nullptr},
    {"security.banlist_path", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.banlistPath); }, nullptr},
    {"security.allowlist_path", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.allowlistPath); }, nullptr},
    {"security.incoming_bandwidth_bps", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.incomingBandwidthBps); }, nullptr},
    {"security.outgoing_bandwidth_bps", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.outgoingBandwidthBps); }, nullptr},
    {"security.operator_password", ReloadClass::Restart,
     [](const ServerConfig& c) { return secretText(c.security.operatorPassword); }, nullptr},
    {"security.pre_handshake_rate_limit_count", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.preHandshakeRateLimitCount); }, nullptr},
    {"security.pre_handshake_window_ms", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.preHandshakeWindowMs); }, nullptr},
    {"security.max_connections_per_ip", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.maxConnectionsPerIp); }, nullptr},
    {"security.admin_auth_max_failures", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.adminAuthMaxFailures); }, nullptr},
    {"security.admin_auth_lockout_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.adminAuthLockoutSeconds); }, nullptr},
    {"security.idle_timeout_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.idleTimeoutS); }, nullptr},
    {"security.seat_request_rate_limit_per_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.seatRequestRateLimitPerS); }, nullptr},
    {"security.team_switch_cooldown_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.teamSwitchCooldownS); }, nullptr},
    {"security.heartbeat_rate_limit_per_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.security.heartbeatRateLimitPerS); }, nullptr},
    {"rcon.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.rcon.enabled); }, nullptr},
    {"rcon.port", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.rcon.port); }, nullptr},
    {"rcon.password", ReloadClass::Restart, [](const ServerConfig& c) { return secretText(c.rcon.password); }, nullptr},
    {"rcon.max_auth_failures", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.rcon.maxAuthFailures); }, nullptr},
    {"rcon.lockout_seconds", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.rcon.lockoutSeconds); }, nullptr},
    {"http_admin.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.httpAdmin.enabled); },
     nullptr},
    {"http_admin.port", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.httpAdmin.port); },
     nullptr},
    {"http_admin.bind_address", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.httpAdmin.bindAddress); }, nullptr},
    {"http_admin.max_auth_failures", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.httpAdmin.maxAuthFailures); }, nullptr},
    {"http_admin.lockout_seconds", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.httpAdmin.lockoutSeconds); }, nullptr},
    {"metrics.tick_json_path", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.metrics.tickJsonPath); }, nullptr},
    {"metrics.tick_json_interval_ms", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.metrics.tickJsonIntervalMs); }, nullptr},
    // [persistence] (#533) — every key is Restart. The store's connections, schema and writer
    // thread are all established at startup, and re-pointing a live server at a different database
    // mid-match would leave half a match's writes in one store and half in another. `reload_config`
    // NAMES a changed key here rather than applying it, which is the whole point of the class.
    {"persistence.enabled", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.persistence.enabled); }, nullptr},
    {"persistence.backend", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.persistence.backend); }, nullptr},
    {"persistence.sqlite_path", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.persistence.sqlitePath); }, nullptr},
    // Reported as a redaction, not as the DSN. `reload_config` prints `key (old -> new)` to every
    // admin frontend, and a connection string carries a password -- so the one place this value is
    // routinely displayed is the one place it must not be displayed verbatim.
    {"persistence.postgres_dsn", ReloadClass::Restart,
     [](const ServerConfig& c) { return c.persistence.postgresDsn.empty()
                                            ? valueText(c.persistence.postgresDsn)
                                            : std::string(fl::persist::redactDsn(c.persistence.postgresDsn)); },
     nullptr},
    {"persistence.busy_timeout_ms", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.persistence.busyTimeoutMs); }, nullptr},
    {"persistence.write_queue_max", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.persistence.writeQueueMax); }, nullptr},
    {"wind.profile_path", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.wind.profilePath); },
     nullptr},
    {"trace.input_trace_dir", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.trace.inputTraceDir); }, nullptr},
    {"replay.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.replay.enabled); },
     nullptr},
    {"replay.dir", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.replay.dir); }, nullptr},
    {"replay.keyframe_interval_ticks", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.replay.keyframeIntervalTicks); }, nullptr},
    {"replay.max_file_mb", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.replay.maxFileMb); },
     nullptr},
    {"replay.max_files", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.replay.maxFiles); },
     nullptr},
    {"replay.hash_log", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.replay.hashLog); },
     nullptr},
    {"spawn.agl_offset", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.spawn.aglOffset); },
     nullptr},
    {"flight.size", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.flight.size); }, nullptr},
    {"flight.entity_type", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.flight.entityType); },
     nullptr},
    {"flight.lateral_m", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.flight.lateralM); },
     nullptr},
    {"flight.aft_m", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.flight.aftM); }, nullptr},
    {"flight.vertical_m", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.flight.verticalM); },
     nullptr},
    {"flight.engage_range_m", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.flight.engageRangeM); }, nullptr},
    {"flight.cover_range_m", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.flight.coverRangeM); }, nullptr},
    {"flight.designate_range_m", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.flight.designateRangeM); }, nullptr},
    {"flight.designate_half_angle_deg", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.flight.designateHalfAngleDeg); }, nullptr},
    {"flight.command_rate_limit_per_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.flight.commandRateLimitPerS); }, nullptr},
    {"atc.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.atc.enabled); }, nullptr},
    {"atc.scramble_entity_type", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.atc.scrambleEntityType); }, nullptr},
    {"chat.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.chat.enabled); }, nullptr},
    {"chat.rate_limit_per_s", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.chat.rateLimitPerS); }, nullptr},
    {"voice.enabled", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.voice.enabled); }, nullptr},
    {"voice.frame_rate_limit", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.voice.frameRateLimit); }, nullptr},
    {"network.transport", ReloadClass::Restart, [](const ServerConfig& c) { return valueText(c.network.transport); },
     nullptr},
    {"network.allow_insecure", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.network.allowInsecure); }, nullptr},
    {"network.compress_snapshots", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.network.compressSnapshots); }, applySnapshotCompression},
    {"network.gns_nagle_time_us", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.network.gnsNagleTimeUs); }, nullptr},
});

} // namespace

std::span<const ConfigKeyInfo> configKeyTable() {
    return kTable;
}

const ConfigKeyInfo* findConfigKey(std::string_view key) {
    const auto it = std::find_if(kTable.begin(), kTable.end(), [key](const ConfigKeyInfo& r) { return r.key == key; });
    return it == kTable.end() ? nullptr : &*it;
}

std::span<const std::string_view> rejectedConfigKeys() {
    return kRejected;
}

std::size_t hotKeyCount() {
    return static_cast<std::size_t>(std::count_if(kTable.begin(), kTable.end(),
                                                  [](const ConfigKeyInfo& r) { return r.reload == ReloadClass::Hot; }));
}

void applyHotKeys(const ReloadApplyContext& rc) {
    for (const ConfigKeyInfo& row : kTable)
        if (row.reload == ReloadClass::Hot)
            row.apply(rc);
}

namespace {
std::vector<ConfigKeyDiff> diffsWhere(const ServerConfig& running, const ServerConfig& incoming, ReloadClass cls) {
    std::vector<ConfigKeyDiff> out;
    for (const ConfigKeyInfo& row : kTable) {
        if (row.reload != cls)
            continue;
        // Compared against the config the process STARTED with, not against the last reload: the
        // question an operator is asking is "will this file's value be in effect?", and for a
        // restart-only key the answer stays no until the restart.
        std::string was = row.read(running);
        std::string now = row.read(incoming);
        if (was != now)
            out.push_back(ConfigKeyDiff{row.key, std::move(was), std::move(now)});
    }
    return out;
}
} // namespace

std::vector<ConfigKeyDiff> changedHotKeys(const ServerConfig& running, const ServerConfig& incoming) {
    return diffsWhere(running, incoming, ReloadClass::Hot);
}

std::vector<ConfigKeyDiff> restartOnlyDiffs(const ServerConfig& running, const ServerConfig& incoming) {
    return diffsWhere(running, incoming, ReloadClass::Restart);
}

} // namespace fl
