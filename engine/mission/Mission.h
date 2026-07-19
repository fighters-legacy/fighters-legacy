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

// A per-seat override within a mission object's `crew:` block (#976). A seat is named by `seatIndex`
// OR `role` (the parser resolves a role to its index against the entity's declared [[crew]]; exactly
// one must be set). Any unset optional keeps the seat's authored default.
struct MissionCrewSeat {
    int seatIndex{-1};                  // authored seat index, or -1 = name the seat by role instead
    std::string role;                   // alternative to seatIndex; resolved to an index at validate time
    std::optional<std::string> botSpec; // override the seat's bot spec ("gunner" / "builtin:gunner" / …)
    std::optional<float> skillMin;      // per-seat skill override (fixed skill = min == max)
    std::optional<float> skillMax;
    std::optional<bool> empty; // occupancy override: true = spawn the seat empty; false = spawn its bot
};

// A mission object's `crew:` block (#976): an aircraft-level skill range that all bot seats roll within
// (seeding the deterministic per-instance skill roll, #971), plus per-seat overrides.
struct MissionCrew {
    std::optional<float> skillMin; // aircraft-level skill range [min, max]; absent = seat defaults
    std::optional<float> skillMax;
    std::vector<MissionCrewSeat> seats; // per-seat overrides
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

    // Crew configuration (#976): bot skill ranges + per-seat overrides for a multi-crew aircraft. The
    // engine-mission parser validates its SHAPE; the caller (fl-server onSpawned) applies it at spawn,
    // and validate-mission --pack cross-checks seats/roles against the entity's declared [[crew]].
    std::optional<MissionCrew> crew;
};

// A win/loss/event condition. `doAction` is the YAML `do:` field (a keyword in C++).
struct MissionTrigger {
    std::string on;
    std::string doAction;
};

// ── Cinematic cameras (#910) ──────────────────────────────────────────────────────────────────
// The mission's optional `cameras:` block is PRESENTATION-ONLY: the server parses and ignores it
// (applyMission never reads shots), the recording client's ShotDirector (#911) consumes it. It lives
// in the engine model so validate-mission covers the schema for free (single schema owner). No
// yaml-cpp types leak into this header — the parser owns yaml-cpp PRIVATE.

enum class ShotType { Static, Orbit, Chase, Move };

// Eye-position easing for a `move` shot; Smooth interpolates Catmull-Rom when > 2 keyframes.
enum class ShotEase { Linear, Smooth };

// One keyframe of a `move` shot: a world-space eye position at `timeSec` relative to the shot start.
struct ShotKeyframe {
    double timeSec{0.0};
    double pos[3]{};
};

// A single camera shot. A variant-free flat struct: ShotDirector switches on `type` and reads only
// the fields relevant to it. Times are sim-seconds from mission start; shots are non-overlapping and
// in ascending `startSec` order (enforced by the parser).
struct MissionShot {
    ShotType type{ShotType::Static};
    double startSec{0.0};
    double durationSec{0.0};
    float fovYDeg{60.f}; // default 60, clamped [20, 120]

    // static: fixed eye anchor (world metres, Y up; an `alt` override folds into pos[1]).
    double pos[3]{};

    // look-at: either a mission object id (lookAtId non-empty) OR a fixed world point (lookAtPointSet).
    // For orbit/chase, both empty/unset means "look at target".
    std::string lookAtId;
    double lookAtPoint[3]{};
    bool lookAtPointSet{false};

    // orbit / chase: the tracked mission object id.
    std::string targetId;

    // orbit params
    double orbitRadiusM{300.0};
    double orbitHeightM{50.0};
    double orbitPeriodSec{30.0}; // seconds per revolution; negative = clockwise

    // chase params: offset in the target's body frame [aft, up, right].
    double chaseOffset[3]{-60.0, 15.0, 0.0};
    double chaseStiffness{0.0}; // 1/s exponential eye smoothing; 0 = rigid

    // move params
    std::vector<ShotKeyframe> keyframes;
    ShotEase ease{ShotEase::Linear};
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
    std::vector<MissionShot> shots; // optional `cameras:` block (#910); empty = no scripted cameras
};

} // namespace fl
