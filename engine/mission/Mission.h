// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The runtime mission model — the parsed, engine-side representation of a mission YAML file
// (schema: docs/modding/missions.md). It lives in the ENGINE (not fl-server) because the game
// client needs the same runtime for the single-player mission lifecycle (#634) and Instant Action
// (#40); fl-server merely wires it at startup (#854). Same promotion rationale as ContentBootstrap.
//
// This model is produced by parseMission() (MissionParser.h) — the single schema owner that
// validate-mission also delegates to, so the linter and the engine cannot drift.

#include "weather/WeatherTypes.h" // WeatherPreset

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// Local mission start time (24-hour clock).
struct MissionTime {
    int hour{0};
    int minute{0};
};

// Steady-state wind: meteorological FROM direction (degrees) + speed (m/s). Gusts are added by
// the engine on top of this.
struct MissionWind {
    float headingDeg{0.f};
    float speedMs{0.f};
};

// A coalition/side. `allies` names the OTHER sides this side is friendly with (symmetric); any
// distinct non-allied side is hostile by the wargame default. A flat `sides: [nato, russia]` list
// yields sides with empty `allies` (two mutually hostile coalitions).
struct MissionSide {
    std::string id;
    std::vector<std::string> allies;
};

// One entity placement at mission start.
struct MissionObject {
    std::string type; // aircraft/unit type id — resolved against the EntityTypeRegistry at spawn
    std::string id;   // unique across the file; referenced by triggers
    std::string side; // must appear in the sides list
    double pos[3]{};  // world-space [x, y, z] metres, Y up
    float headingDeg{0.f};
    std::optional<float> alt;   // MSL altitude (m); overrides pos[1] when present
    std::optional<float> speed; // initial airspeed (m/s) along heading; absent = a sane cruise default
                                // for an airborne start, or 0 for a ground start (#883/#885)
    bool groundStart{false};    // `start: ground` (#885): spawn parked on the ground, gear down, idle
                                // throttle, zero airspeed, held stable until the pilot rotates. Default
                                // `start: air` = dropped in at altitude with a cruise airspeed.
    // `player: true` marks a JOINABLE player slot: applyMission does NOT spawn it as a world entity;
    // it becomes a PlayerSlot the connect handshake assigns a pilot to (#854).
    bool playerSlot{false};

    // Scripted-bot fields (#855). The engine-mission parser validates their SHAPE; the caller
    // (fl-server) turns them into controllers/loadouts via the applyMission onSpawned hook, since that
    // needs engine-ai / engine-script / the weapon registry, which engine-mission does not link.
    //   ai:      a controller spec, e.g. "lua fighter" or a C++ behavior + args like "pursuit 1"
    //            (the same grammar as the `spawn --ai` admin command / AiControllerFactory).
    //   route:   a waypoint list -> a WaypointController (takes precedence over `ai` when both given).
    //   loadout: per-station store ids overriding the entity def's default payload ("~"/"-"/"" = empty).
    std::string ai;
    std::vector<std::array<double, 3>> route;
    std::vector<std::string> loadout;
};

// A win/loss/event condition. `doAction` is the YAML `do:` field (a keyword in C++).
struct MissionTrigger {
    std::string on;
    std::string doAction;
};

// The whole mission.
struct Mission {
    std::string name;
    std::string map;
    std::string layer;
    MissionTime time;
    MissionWind wind;
    std::optional<WeatherPreset> weatherPreset; // absent = engine default (Clear)
    std::optional<float> timeScale;             // absent = server default
    std::vector<MissionSide> sides;
    std::vector<MissionObject> objects;
    std::vector<MissionTrigger> triggers;
};

} // namespace fl
