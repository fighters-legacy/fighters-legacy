// SPDX-License-Identifier: GPL-3.0-or-later
#include "server_config.h"
#include <cstdio>
#include <cstring>
#include <toml++/toml.hpp>

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

    } catch (const toml::parse_error& e) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "failed to parse config: %s -- using defaults", e.what());
        log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        return ServerConfig{};
    }
    return cfg;
}
