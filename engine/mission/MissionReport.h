// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The outcome report a headless mission run writes (#856). fl-server --mission-report <path> runs a
// mission to completion with no clients and writes this as JSON (atomic, via writeConfigFile), and a
// ctest/CI wrapper asserts on it — turning the mission format into the engine's integration-test
// surface. The shape mirrors ServerTickReport (a flat, name-keyed POD + a hand-rolled toJson), so the
// Python side reads it the same way.

#include <cstdint>
#include <cstdio>
#include <string>

namespace fl {

struct MissionReport {
    std::string missionName;
    std::string outcome{"incomplete"}; // "success" | "failure" | "incomplete"
    double elapsedSeconds{0.0};
    uint64_t ticks{0};          // sim ticks stepped before the mission ended (or the cap)
    uint32_t triggersFired{0};  // triggers that fired over the run
    uint32_t liveEntities{0};   // survivors at the end (EntityManager::liveCount)
    uint64_t spawnedObjects{0}; // mission objects successfully spawned at setup
    // Spawns the entity soft cap refused during the run (#1049). A headless run that quietly lost
    // half its objects to a cap otherwise reports an under-populated world as a legitimate result —
    // the class of silently-wrong number a measurement harness has to guard against in the harness.
    uint64_t entityCapRefusals{0};
};

// Minimal, deterministic JSON encoder. Numbers are printed plainly; strings are assumed to contain no
// characters needing escaping (mission names / the fixed outcome vocabulary), matching how the tick
// report treats its keys.
inline std::string toJson(const MissionReport& r) {
    auto num = [](double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        return std::string(buf);
    };
    std::string s = "{\n";
    s += "  \"mission_name\": \"" + r.missionName + "\",\n";
    s += "  \"outcome\": \"" + r.outcome + "\",\n";
    s += "  \"elapsed_seconds\": " + num(r.elapsedSeconds) + ",\n";
    s += "  \"ticks\": " + std::to_string(r.ticks) + ",\n";
    s += "  \"triggers_fired\": " + std::to_string(r.triggersFired) + ",\n";
    s += "  \"live_entities\": " + std::to_string(r.liveEntities) + ",\n";
    s += "  \"spawned_objects\": " + std::to_string(r.spawnedObjects) + ",\n";
    s += "  \"entity_cap_refusals\": " + std::to_string(r.entityCapRefusals) + "\n";
    s += "}";
    return s;
}

} // namespace fl
