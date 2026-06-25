// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/BreakTurnController.h"
#include "ai/EvadeController.h"
#include "ai/LoiterController.h"
#include "ai/PursuitController.h"
#include "ai/StateMachineController.h" // exposes Condition helpers + StateMachineController
#include "ai/WaypointController.h"
#include "entity/EntityManager.h"

#include <charconv>
#include <cstdlib>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl::ai {

// Creates an AI controller from a behavior name and its arguments.
// entityManager is required for "pursuit", "evade", and "break" behaviors.
// Returns nullptr on unknown behavior, parse error, or missing entity.
//
// Behaviors and their args:
//   loiter   [cx cy cz [radius_m [alt_m [throttle [cw|ccw]]]]]
//   waypoint  x1 y1 z1 [x2 y2 z2 ...] [--loop]
//   pursuit   <entityIdx>
//   evade     <entityIdx>
//   break     <entityIdx> [rollDurationS]
//
// For composed multi-state behaviors (patrol-attack-retreat, escort, etc.)
// build a StateMachineController directly in C++, or use LuaController for
// script-driven behavior — neither is expressible as flat string args.
inline std::unique_ptr<fl::IEntityController> createController(std::string_view behavior,
                                                               std::span<std::string_view> args,
                                                               const fl::EntityManager* entityManager = nullptr) {
    // Parse a double from a string_view using strtod.
    // from_chars for floating-point is not available on Apple Clang.
    auto parseDouble = [](std::string_view sv, double& out) -> bool {
        if (sv.empty())
            return false;
        std::string tmp(sv);
        char* end = nullptr;
        out = std::strtod(tmp.c_str(), &end);
        return end != tmp.c_str() && end == tmp.c_str() + sv.size();
    };

    // Parse a uint32_t from a string_view using from_chars (integer support on all platforms).
    auto parseUint32 = [](std::string_view sv, uint32_t& out) -> bool {
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
        return ec == std::errc{} && ptr == sv.data() + sv.size();
    };

    // Find a live (non-dead) entity by pool index, returning its EntityId.
    // Must be called on the sim thread (entityManager->forEach is sim-thread only).
    auto findEntityById = [&](uint32_t idx) -> fl::EntityId {
        if (!entityManager)
            return fl::EntityId::null();
        fl::EntityId found;
        entityManager->forEach([&](const fl::EntityState& s) {
            if (!found.valid() && !s.dead && s.id.index == idx)
                found = s.id;
        });
        return found;
    };

    // -----------------------------------------------------------------------
    // loiter [cx cy cz [radius_m [alt_m [throttle [cw|ccw]]]]]
    // -----------------------------------------------------------------------
    if (behavior == "loiter") {
        glm::dvec3 center{0.0, 600.0, 0.0};
        float radius = 3000.f;
        float alt = 600.f;
        float thr = 0.65f;
        LoiterDir dir = LoiterDir::Clockwise;

        double d = 0.0;
        if (args.size() >= 3) {
            if (!parseDouble(args[0], d))
                return nullptr;
            center.x = d;
            if (!parseDouble(args[1], d))
                return nullptr;
            center.y = d;
            if (!parseDouble(args[2], d))
                return nullptr;
            center.z = d;
        }
        if (args.size() >= 4) {
            if (!parseDouble(args[3], d))
                return nullptr;
            radius = static_cast<float>(d);
        }
        if (args.size() >= 5) {
            if (!parseDouble(args[4], d))
                return nullptr;
            alt = static_cast<float>(d);
        }
        if (args.size() >= 6) {
            if (!parseDouble(args[5], d))
                return nullptr;
            thr = static_cast<float>(d);
        }
        if (args.size() >= 7) {
            if (args[6] == "ccw")
                dir = LoiterDir::CounterClockwise;
            else if (args[6] == "cw")
                dir = LoiterDir::Clockwise;
            else
                return nullptr;
        }
        return std::make_unique<LoiterController>(center, radius, alt, thr, dir);
    }

    // -----------------------------------------------------------------------
    // waypoint  x1 y1 z1 [x2 y2 z2 ...] [--loop]
    // -----------------------------------------------------------------------
    if (behavior == "waypoint") {
        bool loop = false;
        std::vector<std::string_view> coordArgs;
        coordArgs.reserve(args.size());
        for (auto& a : args) {
            if (a == "--loop")
                loop = true;
            else
                coordArgs.push_back(a);
        }
        if (coordArgs.empty() || coordArgs.size() % 3 != 0)
            return nullptr;

        std::vector<glm::dvec3> wps;
        wps.reserve(coordArgs.size() / 3);
        for (std::size_t i = 0; i < coordArgs.size(); i += 3) {
            double wx{}, wy{}, wz{};
            if (!parseDouble(coordArgs[i], wx))
                return nullptr;
            if (!parseDouble(coordArgs[i + 1], wy))
                return nullptr;
            if (!parseDouble(coordArgs[i + 2], wz))
                return nullptr;
            wps.push_back({wx, wy, wz});
        }
        return std::make_unique<WaypointController>(std::move(wps), 500.f, 0.7f, loop);
    }

    // -----------------------------------------------------------------------
    // pursuit  <entityIdx>
    // -----------------------------------------------------------------------
    if (behavior == "pursuit") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;
        return std::make_unique<PursuitController>(*entityManager, id);
    }

    // -----------------------------------------------------------------------
    // evade  <entityIdx>
    // -----------------------------------------------------------------------
    if (behavior == "evade") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;
        return std::make_unique<EvadeController>(*entityManager, id);
    }

    // -----------------------------------------------------------------------
    // break  <entityIdx> [rollDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "break") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float rollDur = 0.5f;
        if (args.size() >= 2) {
            double d2 = 0.0;
            if (!parseDouble(args[1], d2))
                return nullptr;
            rollDur = static_cast<float>(d2);
        }
        return std::make_unique<BreakTurnController>(*entityManager, id, rollDur);
    }

    return nullptr;
}

} // namespace fl::ai
