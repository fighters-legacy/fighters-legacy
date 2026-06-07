// SPDX-License-Identifier: GPL-3.0-or-later
#include "server_config.h"
#include <cstdio>
#include <cstring>
#include <toml++/toml.hpp>

// ---------------------------------------------------------------------------
// Default configuration template
// ---------------------------------------------------------------------------

static const char* kDefaultToml =
    "[server]\n"
    "# Human-readable server name shown in the lobby browser.\n"
    "name = \"Unnamed Server\"\n"
    "\n"
    "# UDP port fl-server binds on. Port 4778 is the fighters-legacy default.\n"
    "# See IANA registration note in docs/architecture.md.\n"
    "port = 4778\n"
    "\n"
    "# Network interface to bind on.\n"
    "# \"::\"         = dual-stack all interfaces (IPv4+IPv6; recommended for internet servers)\n"
    "# \"0.0.0.0\"   = IPv4 all interfaces\n"
    "# \"127.0.0.1\" = localhost-only IPv4 (single-player; game client uses this)\n"
    "# \"::1\"        = localhost-only IPv6\n"
    "bind_address = \"0.0.0.0\"\n"
    "\n"
    "# Maximum number of simultaneous connected peers (1-128).\n"
    "max_peers = 16\n"
    "\n"
    "# Scenario types this server will host.\n"
    "# Valid values: \"campaign\", \"mission\", \"sandbox\"\n"
    "game_modes = [\"campaign\", \"mission\", \"sandbox\"]\n"
    "\n"
    "# Message shown to connecting clients. Empty string = no message.\n"
    "motd = \"\"\n"
    "\n"
    "# Server password. Empty string = no password required.\n"
    "password = \"\"\n"
    "\n"
    "[rotation]\n"
    "order = \"sequential\"\n"
    "items = []\n"
    "time_limit_min = 0\n"
    "\n"
    "[lobby]\n"
    "register = false\n"
    "url = \"https://lobby.fighters-legacy.org\"\n"
    "visibility = \"public\"\n"
    "\n"
    "[mods]\n"
    "stack = []\n"
    "\n"
    "[world]\n"
    "save_path = \"world.sav\"\n"
    "autosave_interval_s = 300\n"
    "\n"
    "[ai]\n"
    "difficulty_floor = \"recruit\"\n"
    "\n"
    "[discovery]\n"
    "# LAN server discovery beacon.\n"
    "enabled = true\n"
    "interval_ms = 2000\n"
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
    "[shutdown]\n"
    "shutdown_warning_interval_s = 300\n"
    "min_shutdown_delay_s = 0\n"
    "shutdown_require_confirm = true\n";

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

ServerConfig parseServerConfig(std::string_view content, ILogger* log) {
    ServerConfig cfg;
    try {
        auto tbl = toml::parse(content);

        // [server]
        if (auto v = tbl["server"]["name"].value<std::string>())
            cfg.name = std::move(*v);
        if (auto v = tbl["server"]["port"].value<int64_t>()) {
            if (*v < 1 || *v > 65535) {
                log->log(LogLevel::Warn, __FILE__, __LINE__, "server.port out of range [1,65535]; using default");
            } else {
                cfg.port = static_cast<uint16_t>(*v);
            }
        }
        if (auto v = tbl["server"]["bind_address"].value<std::string>())
            cfg.bindAddress = std::move(*v);
        if (auto v = tbl["server"]["max_peers"].value<int64_t>()) {
            if (*v < 1 || *v > 128) {
                log->log(LogLevel::Warn, __FILE__, __LINE__, "server.max_peers out of range [1,128]; using default");
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
        if (auto v = tbl["rotation"]["time_limit_min"].value<int64_t>())
            cfg.rotationTimeLimitMin = static_cast<int>(*v);

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

        // [world]
        if (auto v = tbl["world"]["save_path"].value<std::string>())
            cfg.worldSavePath = std::move(*v);
        if (auto v = tbl["world"]["autosave_interval_s"].value<int64_t>())
            cfg.worldAutosaveIntervalS = static_cast<int>(*v);
        if (auto v = tbl["world"]["entity_soft_cap"].value<int64_t>()) {
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

        // [ai]
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
        if (auto v = tbl["discovery"]["interval_ms"].value<int64_t>()) {
            if (*v < 100 || *v > 60000) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "discovery.interval_ms out of range [100,60000]; using default");
            } else {
                cfg.discoveryIntervalMs = static_cast<int>(*v);
            }
        }

        // [security]
        if (auto v = tbl["security"]["connect_rate_limit_count"].value<int64_t>()) {
            if (*v < 1 || *v > 100) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.connect_rate_limit_count out of range [1,100]; using default");
            } else {
                cfg.connectRateLimitCount = static_cast<int>(*v);
            }
        }
        if (auto v = tbl["security"]["connect_rate_limit_window_s"].value<int64_t>()) {
            if (*v < 1 || *v > 3600) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.connect_rate_limit_window_s out of range [1,3600]; using default");
            } else {
                cfg.connectRateLimitWindowS = static_cast<int>(*v);
            }
        }
        if (auto v = tbl["security"]["packet_flood_multiplier"].value<int64_t>()) {
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
        if (auto v = tbl["security"]["incoming_bandwidth_bps"].value<int64_t>()) {
            if (*v < 0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.incoming_bandwidth_bps must be >= 0; using 0 (unlimited)");
            } else {
                cfg.incomingBandwidthBps = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tbl["security"]["outgoing_bandwidth_bps"].value<int64_t>()) {
            if (*v < 0) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.outgoing_bandwidth_bps must be >= 0; using 0 (unlimited)");
            } else {
                cfg.outgoingBandwidthBps = static_cast<uint32_t>(*v);
            }
        }
        if (auto v = tbl["security"]["operator_password"].value<std::string>())
            cfg.operatorPassword = std::move(*v);
        if (auto v = tbl["security"]["pre_handshake_rate_limit_count"].value<int64_t>()) {
            if (*v < 0 || *v > 10000) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.pre_handshake_rate_limit_count out of range [0,10000]; using default");
            } else {
                cfg.preHandshakeRateLimitCount = static_cast<int>(*v);
            }
        }
        if (auto v = tbl["security"]["pre_handshake_window_ms"].value<int64_t>()) {
            if (*v < 100 || *v > 60000) {
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "security.pre_handshake_window_ms out of range [100,60000]; using default");
            } else {
                cfg.preHandshakeWindowMs = static_cast<int>(*v);
            }
        }

        // [shutdown]
        if (auto v = tbl["shutdown"]["warning_interval_s"].value<int64_t>()) {
            if (*v < 1 || *v > 86400)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "shutdown.warning_interval_s out of range [1,86400]; using default");
            else
                cfg.shutdownWarningIntervalS = static_cast<int>(*v);
        }
        if (auto v = tbl["shutdown"]["min_shutdown_delay_s"].value<int64_t>()) {
            if (*v < 0 || *v > 86400)
                log->log(LogLevel::Warn, __FILE__, __LINE__,
                         "shutdown.min_shutdown_delay_s out of range [0,86400]; using default");
            else
                cfg.minShutdownDelayS = static_cast<int>(*v);
        }
        if (auto v = tbl["shutdown"]["require_confirm"].value<bool>())
            cfg.shutdownRequireConfirm = *v;

    } catch (const toml::parse_error& e) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "failed to parse config: %s -- using defaults", e.what());
        log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        return ServerConfig{};
    }
    return cfg;
}
