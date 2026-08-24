// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/GameModeParser.h"

#include "config/TomlNumeric.h"
#include "util/Str.h" // the one ASCII case rule (#1265)

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>

namespace fl {

namespace {

// Clamp a double into [lo, hi], appending a warning when it was out of range.
double clampD(double v, double lo, double hi, const char* field, std::vector<std::string>& warnings) {
    if (v < lo || v > hi) {
        warnings.push_back(std::string("game mode: '") + field + "' out of range; clamped");
        return std::clamp(v, lo, hi);
    }
    return v;
}

int clampI(int v, int lo, int hi, const char* field, std::vector<std::string>& warnings) {
    if (v < lo || v > hi) {
        warnings.push_back(std::string("game mode: '") + field + "' out of range; clamped");
        return std::clamp(v, lo, hi);
    }
    return v;
}

} // namespace

GameModeParseResult parseGameModeToml(std::string_view tomlContent) {
    GameModeParseResult r;
    toml::table tbl;
    try {
        tbl = toml::parse(tomlContent);
    } catch (const toml::parse_error& e) {
        r.ok = false;
        r.error = std::string("TOML parse error: ") + e.description().data();
        return r;
    }

    GameModeDef& m = r.mode;

    if (const toml::node_view mode = tbl["mode"]) {
        if (auto id = mode["id"].value<std::string>())
            m.id = *id;
        if (auto name = mode["name"].value<std::string>())
            m.name = *name;
    }
    if (m.id.empty()) {
        r.ok = false;
        r.error = "game mode: missing [mode] id";
        return r;
    }

    if (const toml::node_view teams = tbl["teams"]) {
        if (auto ums = teams["use_mission_sides"].value<bool>())
            m.useMissionSides = *ums;
        if (const toml::array* arr = teams["team"].as_array()) {
            for (const toml::node& n : *arr) {
                const toml::table* t = n.as_table();
                if (!t)
                    continue;
                GameModeTeam team;
                if (auto id = (*t)["id"].value<std::string>())
                    team.id = *id;
                if (auto name = (*t)["name"].value<std::string>())
                    team.name = *name;
                if (auto cap = tomlIntNarrow((*t)["capacity"]))
                    team.capacity = clampI(*cap, 0, 4096, "team.capacity", r.warnings);
                if (team.id.empty()) {
                    r.warnings.push_back("game mode: a [[teams.team]] has no id; skipped");
                    continue;
                }
                m.teams.push_back(std::move(team));
            }
        }
    }
    // use_mission_sides=true AND an explicit team list is contradictory; the mission's sides win.
    if (m.useMissionSides && !m.teams.empty())
        r.warnings.push_back("game mode: use_mission_sides is true but [[teams.team]] entries were "
                             "given; the mission's sides are used and the team list is ignored");

    if (const toml::node_view sc = tbl["scoring"]) {
        if (auto v = tomlIntNarrow(sc["points_per_kill"]))
            m.pointsPerKill = clampI(*v, -1000, 1000, "points_per_kill", r.warnings);
        if (auto v = tomlIntNarrow(sc["points_per_assist"]))
            m.pointsPerAssist = clampI(*v, -1000, 1000, "points_per_assist", r.warnings);
        if (auto v = tomlIntNarrow(sc["points_per_objective"]))
            m.pointsPerObjective = clampI(*v, -100000, 100000, "points_per_objective", r.warnings);
        if (auto v = tomlIntNarrow(sc["score_limit"]))
            m.scoreLimit = clampI(*v, 0, 1000000, "score_limit", r.warnings);
    }

    if (const toml::node_view mt = tbl["match"]) {
        if (auto v = mt["time_limit_min"].value<double>())
            m.timeLimitS = clampD(*v, 0.0, 1440.0, "time_limit_min", r.warnings) * 60.0;
        if (auto v = mt["warmup_s"].value<double>())
            m.warmupS = clampD(*v, 0.0, 3600.0, "warmup_s", r.warnings);
        if (auto v = tomlIntNarrow(mt["min_players"]))
            m.minPlayers = clampI(*v, 1, 4096, "min_players", r.warnings);
    }

    if (const toml::node_view rs = tbl["respawn"]) {
        if (auto v = rs["delay_s"].value<double>())
            m.respawnDelayS = clampD(*v, 0.0, 3600.0, "respawn.delay_s", r.warnings);
        if (auto v = rs["waves"].value<bool>())
            m.respawnWaves = *v;
        if (auto v = rs["wave_interval_s"].value<double>())
            m.waveIntervalS = clampD(*v, 1.0, 600.0, "respawn.wave_interval_s", r.warnings);
    }

    if (const toml::node_view rules = tbl["rules"]) {
        if (auto ff = rules["friendly_fire"].value<std::string>()) {
            const std::string v = asciiLower(*ff);
            if (v == "server")
                m.friendlyFire = ModeFriendlyFire::Server;
            else if (v == "on" || v == "true")
                m.friendlyFire = ModeFriendlyFire::On;
            else if (v == "off" || v == "false")
                m.friendlyFire = ModeFriendlyFire::Off;
            else
                r.warnings.push_back("game mode: unknown friendly_fire '" + *ff + "'; using server default");
        }
    }

    r.ok = true;
    return r;
}

} // namespace fl
