// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/BallisticGuidanceController.h"
#include "ai/BreakTurnController.h"
#include "ai/DynamicLoiterController.h"
#include "ai/EvadeController.h"
#include "ai/FormationController.h"
#include "ai/GunsEmploymentController.h"
#include "ai/HighYoYoController.h"
#include "ai/ImmelmannController.h"
#include "ai/LagPursuitController.h"
#include "ai/LeadPursuitController.h"
#include "ai/LoiterController.h"
#include "ai/LowYoYoController.h"
#include "ai/PursuitController.h"
#include "ai/SplitSController.h"
#include "ai/StateMachineController.h"   // exposes Condition helpers + StateMachineController
#include "ai/SurfaceThreatControllers.h" // sam / aaa emplacements (#863)
#include "ai/SwarmController.h"          // boids swarm member (#353)
#include "ai/WaypointController.h"
#include "ai/WingmanBehavior.h" // makeWingmanController + WingmanParams
#include "ai/WingmanCommand.h"  // the six-command grammar
#include "entity/EntityManager.h"

#include <charconv>
#include <cstdlib>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl::ai {

// Creates an AI controller from a behavior name and its arguments.
// entityManager is required for entity-targeting behaviors. Returns nullptr on unknown
// behavior, parse error, or missing entity.
//
// Single-state behaviors:
//   loiter        [cx cy cz [radius_m [alt_m [throttle [cw|ccw]]]]]
//   dynamic_loiter <entityIdx> [radius_m [throttle [cw|ccw]]]  — orbits a MOVING entity (#464)
//   waypoint      x1 y1 z1 [x2 y2 z2 ...] [--loop]
//   pursuit       <entityIdx>
//   evade         <entityIdx>
//   break         <entityIdx> [rollDurationS]
//   lead          <entityIdx> [navGain]
//   lag           <entityIdx> [lagFraction]
//   immelmann     [pullDurationS] [rollDurationS]
//   split_s       [rollDurationS] [pullDurationS]
//   high_yo_yo    <entityIdx> [climbDurationS] [reacquireDurationS]
//   low_yo_yo     <entityIdx> [diveDurationS] [pullDurationS]
//   guns          <entityIdx> [muzzleVelMps] [lethalRadiusM]
//   sam           [engageRangeM=30000] [coneHalfDeg=90] [fireIntervalS=4]  — static SAM launcher (#863)
//   aaa           [engageRangeM=1200] [coneHalfDeg=25] [muzzleVelMps=1000] [lethalRadiusM=15]  — static AAA (#863)
//   ballistic     <tx> <ty> <tz> [mirvCount [spreadM]]  — boost-phase steering to an impact point
//   formation     <anchorIdx> [slotIndex=0] [lateralM=150] [aftM=100]   — holds station on a MOVING
//                 anchor (tight formation flying; `escort` orbits the moving asset at standoff)
//   swarm         <cx> <cy> <cz> [neighborRadiusM=600] [separationRadiusM=120] [cruiseThrottle=0.75]
//                 — boids member (#353): separation/alignment/cohesion with same-type same-faction
//                 flockmates, migrating toward the point
//   swarm_follow  <anchorIdx> [neighborRadiusM=600] [separationRadiusM=120] [cruiseThrottle=0.75]
//                 — same boids member, migrating after a MOVING anchor entity
//
// StateMachineController templates (internally compose multiple states):
//   patrol_attack <entityIdx> [engageRangeM=8000] [retreatHp=0.25]
//   escort        <entityIdx> [standoffM=2000]
//   wingman       <anchorIdx> <command> [slotIndex=0]  — one of the six scripted wingman commands
//                 (WingmanCommand.h); the same code the network order path drives, so a console
//                 order and a radio order cannot behave differently
//
// Custom multi-state scenarios not covered by these templates must be constructed
// in C++ via StateMachineController directly. Lua behavior via LuaController.
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
    // dynamic_loiter  <entityIdx> [radius_m [throttle [cw|ccw]]]
    // Orbits a MOVING entity (re-centers each tick), unlike the fixed-center loiter above.
    // -----------------------------------------------------------------------
    if (behavior == "dynamic_loiter") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float radius = 3000.f;
        float thr = 0.65f;
        LoiterDir dir = LoiterDir::Clockwise;
        double d = 0.0;
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d))
                return nullptr;
            radius = static_cast<float>(d);
        }
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d))
                return nullptr;
            thr = static_cast<float>(d);
        }
        if (args.size() >= 4) {
            if (args[3] == "ccw")
                dir = LoiterDir::CounterClockwise;
            else if (args[3] == "cw")
                dir = LoiterDir::Clockwise;
            else
                return nullptr;
        }
        return std::make_unique<DynamicLoiterController>(*entityManager, id, radius, thr, dir);
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

    // -----------------------------------------------------------------------
    // lead  <entityIdx> [navGain]
    // -----------------------------------------------------------------------
    if (behavior == "lead") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float navGain = 1.0f;
        if (args.size() >= 2) {
            double d2 = 0.0;
            if (!parseDouble(args[1], d2))
                return nullptr;
            navGain = static_cast<float>(d2);
        }
        return std::make_unique<LeadPursuitController>(*entityManager, id, navGain);
    }

    // -----------------------------------------------------------------------
    // lag  <entityIdx> [lagFraction]
    // -----------------------------------------------------------------------
    if (behavior == "lag") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float lagFraction = 1.0f;
        if (args.size() >= 2) {
            double d2 = 0.0;
            if (!parseDouble(args[1], d2))
                return nullptr;
            lagFraction = static_cast<float>(d2);
        }
        return std::make_unique<LagPursuitController>(*entityManager, id, lagFraction);
    }

    // -----------------------------------------------------------------------
    // immelmann  [pullDurationS] [rollDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "immelmann") {
        float pullDur = 4.0f;
        float rollDur = 1.5f;
        double d = 0.0;
        if (args.size() >= 1) {
            if (!parseDouble(args[0], d))
                return nullptr;
            pullDur = static_cast<float>(d);
        }
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d))
                return nullptr;
            rollDur = static_cast<float>(d);
        }
        return std::make_unique<ImmelmannController>(pullDur, rollDur);
    }

    // -----------------------------------------------------------------------
    // split_s  [rollDurationS] [pullDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "split_s") {
        float rollDur = 1.5f;
        float pullDur = 4.0f;
        double d = 0.0;
        if (args.size() >= 1) {
            if (!parseDouble(args[0], d))
                return nullptr;
            rollDur = static_cast<float>(d);
        }
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d))
                return nullptr;
            pullDur = static_cast<float>(d);
        }
        return std::make_unique<SplitSController>(rollDur, pullDur);
    }

    // -----------------------------------------------------------------------
    // high_yo_yo  <entityIdx> [climbDurationS] [reacquireDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "high_yo_yo") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float climbDur = 2.5f;
        float reacquireDur = 3.0f;
        double d = 0.0;
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d))
                return nullptr;
            climbDur = static_cast<float>(d);
        }
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d))
                return nullptr;
            reacquireDur = static_cast<float>(d);
        }
        return std::make_unique<HighYoYoController>(*entityManager, id, climbDur, reacquireDur);
    }

    // -----------------------------------------------------------------------
    // low_yo_yo  <entityIdx> [diveDurationS] [pullDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "low_yo_yo") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float diveDur = 1.5f;
        float pullDur = 2.5f;
        double d = 0.0;
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d))
                return nullptr;
            diveDur = static_cast<float>(d);
        }
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d))
                return nullptr;
            pullDur = static_cast<float>(d);
        }
        return std::make_unique<LowYoYoController>(*entityManager, id, diveDur, pullDur);
    }

    // -----------------------------------------------------------------------
    // ballistic  <tx> <ty> <tz> [mirvCount [spreadM]]  (#355)
    // -----------------------------------------------------------------------
    if (behavior == "ballistic") {
        if (args.size() < 3)
            return nullptr;
        BallisticGuidanceController::Params p;
        double d = 0.0;
        if (!parseDouble(args[0], d))
            return nullptr;
        p.targetPos.x = d;
        if (!parseDouble(args[1], d))
            return nullptr;
        p.targetPos.y = d;
        if (!parseDouble(args[2], d))
            return nullptr;
        p.targetPos.z = d;
        if (args.size() >= 4) {
            uint32_t n{};
            if (!parseUint32(args[3], n) || n > 64)
                return nullptr;
            p.mirvCount = static_cast<int>(n);
        }
        if (args.size() >= 5) {
            if (!parseDouble(args[4], d) || d < 0.0)
                return nullptr;
            p.mirvSpreadM = d;
        }
        return std::make_unique<BallisticGuidanceController>(p);
    }

    // -----------------------------------------------------------------------
    // guns  <entityIdx> [muzzleVelMps=1030] [lethalRadiusM=8]
    // -----------------------------------------------------------------------
    if (behavior == "guns") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float muzzleVel = 1030.f;
        float lethalRadius = 8.f;
        double d = 0.0;
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d) || d <= 0.0)
                return nullptr;
            muzzleVel = static_cast<float>(d);
        }
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d) || d <= 0.0)
                return nullptr;
            lethalRadius = static_cast<float>(d);
        }
        return std::make_unique<GunsEmploymentController>(*entityManager, id, muzzleVel, lethalRadius);
    }

    // -----------------------------------------------------------------------
    // sam  [engageRangeM=30000] [coneHalfDeg=90] [fireIntervalS=4]   (#863 — static SAM launcher;
    //      auto-engages hostiles it detects on radar, no target index)
    // -----------------------------------------------------------------------
    if (behavior == "sam") {
        if (!entityManager)
            return nullptr;
        float rangeM = 30000.f, coneHalfDeg = 90.f, fireIntervalS = 4.f;
        double d = 0.0;
        if (args.size() >= 1) {
            if (!parseDouble(args[0], d) || d <= 0.0)
                return nullptr;
            rangeM = static_cast<float>(d);
        }
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d) || d <= 0.0)
                return nullptr;
            coneHalfDeg = static_cast<float>(d);
        }
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d) || d <= 0.0)
                return nullptr;
            fireIntervalS = static_cast<float>(d);
        }
        return std::make_unique<SamEngagementController>(*entityManager, rangeM, coneHalfDeg, fireIntervalS);
    }

    // -----------------------------------------------------------------------
    // aaa  [engageRangeM=4000] [coneHalfDeg=25] [muzzleVelMps=1000] [lethalRadiusM=15]   (#863 — static
    //      AAA gun; auto-leads and engages hostiles in its cone)
    // -----------------------------------------------------------------------
    if (behavior == "aaa") {
        if (!entityManager)
            return nullptr;
        float rangeM = 1200.f, coneHalfDeg = 25.f, muzzleVel = 1000.f, lethalRadius = 15.f;
        double d = 0.0;
        if (args.size() >= 1) {
            if (!parseDouble(args[0], d) || d <= 0.0)
                return nullptr;
            rangeM = static_cast<float>(d);
        }
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d) || d <= 0.0)
                return nullptr;
            coneHalfDeg = static_cast<float>(d);
        }
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d) || d <= 0.0)
                return nullptr;
            muzzleVel = static_cast<float>(d);
        }
        if (args.size() >= 4) {
            if (!parseDouble(args[3], d) || d <= 0.0)
                return nullptr;
            lethalRadius = static_cast<float>(d);
        }
        return std::make_unique<AaaFireController>(*entityManager, rangeM, coneHalfDeg, muzzleVel, lethalRadius);
    }

    // -----------------------------------------------------------------------
    // patrol_attack  <entityIdx> [engageRangeM=8000] [retreatHp=0.25]
    // -----------------------------------------------------------------------
    if (behavior == "patrol_attack") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId id = findEntityById(idx);
        if (!id.valid())
            return nullptr;

        float engageRangeM = 8000.f;
        float retreatHp = 0.25f;
        double d = 0.0;
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d))
                return nullptr;
            engageRangeM = static_cast<float>(d);
        }
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d))
                return nullptr;
            retreatHp = static_cast<float>(d);
        }

        // Patrol loiter center: above the target's XZ position at 600 m altitude.
        // Captured at factory-creation time; fixed for the lifetime of this controller.
        const fl::EntityState* ts = entityManager->get(id);
        glm::dvec3 patrolCenter =
            ts ? glm::dvec3(ts->transform.pos[0], 600.0, ts->transform.pos[2]) : glm::dvec3{0.0, 600.0, 0.0};

        auto sm = std::make_unique<StateMachineController>(*entityManager);
        sm->addState("patrol", [patrolCenter]() {
            return std::make_unique<LoiterController>(patrolCenter, 3000.f, 600.f, 0.65f, LoiterDir::Clockwise);
        });
        sm->addState("engage", [entityManager, id]() {
            return std::make_unique<LeadPursuitController>(*entityManager, id, 1.0f, 0.9f, false);
        });
        sm->addState("retreat", [entityManager, id]() {
            return std::make_unique<EvadeController>(*entityManager, id, 1.0f, true);
        });
        // HONEST TRIGGERS (#690). The behavior string is unchanged, so `spawn --ai patrol_attack <idx>`
        // means exactly what it always did to an operator — but the AI now engages only what it has
        // actually DETECTED and REACTED to, and goes back to patrol when it has lost the contact
        // rather than when the target crosses an invisible radius it could never have measured. With
        // no sensing evaluated (unit tests, headless callers) these fall back to their ground-truth
        // ancestors, so nothing that worked before behaves differently.
        sm->addTransition("patrol", "engage", DetectsThreatWithinRange(id, engageRangeM));
        sm->addTransition("engage", "retreat", HpBelow(retreatHp));
        sm->addTransition("engage", "patrol", LostContact(id, engageRangeM * 1.5f), 2.f);
        sm->setInitialState("patrol");
        return sm;
    }

    // -----------------------------------------------------------------------
    // escort  <entityIdx> [standoffM=2000]
    // -----------------------------------------------------------------------
    if (behavior == "escort") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId escortedId = findEntityById(idx);
        if (!escortedId.valid())
            return nullptr;

        float standoffM = 2000.f;
        double d = 0.0;
        if (args.size() >= 2) {
            if (!parseDouble(args[1], d))
                return nullptr;
            standoffM = static_cast<float>(d);
        }

        // The escort orbits the escortee at standoffM radius, tracking it as it MOVES (#464): the
        // follow state is a DynamicLoiterController re-centred each tick on the escortee's live
        // position, not the fixed point it occupied when the order was given. The break transition
        // uses DetectedHostileWithinRange, which ignores the escortee by faction (same faction as the
        // escort) rather than by geometry (#465) — so the escort and escortee must be spawned with
        // the same non-neutral faction, and only a hostile intruder trips the break.
        const float innerRange = standoffM * 0.5f;

        auto sm = std::make_unique<StateMachineController>(*entityManager);
        const fl::EntityManager* em = entityManager;
        sm->addState("follow", [em, escortedId, standoffM]() {
            return std::make_unique<DynamicLoiterController>(*em, escortedId, standoffM, 0.65f, LoiterDir::Clockwise);
        });
        sm->addState("break", []() { return std::make_unique<ImmelmannController>(); });
        // The escort breaks on a hostile it has actually SEEN (#690) — not on one it could not
        // possibly have noticed. Same fallback rule as patrol_attack when sensing is not evaluated.
        sm->addTransition("follow", "break", DetectedHostileWithinRange(innerRange));
        sm->addTransition("break", "follow", Not(DetectedHostileWithinRange(innerRange)), 6.0f);
        sm->setInitialState("follow");
        return sm;
    }

    // -----------------------------------------------------------------------
    // formation <anchorIdx> [slotIndex=0] [lateralM=150] [aftM=100]
    //
    // Hold station on a MOVING anchor. Unlike `escort` above (which orbits the point where the
    // escortee was standing when the order was given), this tracks the anchor continuously — it is
    // the controller `escort`'s own comment says is missing.
    // -----------------------------------------------------------------------
    if (behavior == "formation") {
        if (args.empty() || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId anchorId = findEntityById(idx);
        if (!anchorId.valid())
            return nullptr;

        uint32_t slotIndex = 0;
        FormationParams fp{};
        double d = 0.0;
        if (args.size() >= 2 && !parseUint32(args[1], slotIndex))
            return nullptr;
        if (args.size() >= 3) {
            if (!parseDouble(args[2], d))
                return nullptr;
            fp.lateralM = static_cast<float>(d);
        }
        if (args.size() >= 4) {
            if (!parseDouble(args[3], d))
                return nullptr;
            fp.aftM = static_cast<float>(d);
        }
        return std::make_unique<FormationController>(*entityManager, anchorId, slotIndex, fp);
    }

    // -----------------------------------------------------------------------
    // swarm <cx> <cy> <cz> [...] / swarm_follow <anchorIdx> [...]  (#353)
    //
    // One boids member. Spawn N entities of the same type and faction with this behavior and they
    // flock: separation/alignment/cohesion over the shared spatial index, migrating toward the
    // point (or after the anchor). Each member is independent — there is no swarm object to manage,
    // and losing members degrades the flock, never breaks it.
    // -----------------------------------------------------------------------
    if (behavior == "swarm" || behavior == "swarm_follow") {
        if (!entityManager)
            return nullptr;
        SwarmParams sp{};
        std::size_t tail = 0; // index of the first optional arg
        glm::dvec3 point{};
        fl::EntityId anchorId{};
        if (behavior == "swarm") {
            if (args.size() < 3)
                return nullptr;
            double x = 0.0, y = 0.0, z = 0.0;
            if (!parseDouble(args[0], x) || !parseDouble(args[1], y) || !parseDouble(args[2], z))
                return nullptr;
            point = {x, y, z};
            tail = 3;
        } else {
            if (args.empty())
                return nullptr;
            uint32_t idx{};
            if (!parseUint32(args[0], idx))
                return nullptr;
            anchorId = findEntityById(idx);
            if (!anchorId.valid())
                return nullptr;
            tail = 1;
        }
        double d = 0.0;
        if (args.size() >= tail + 1) {
            if (!parseDouble(args[tail], d))
                return nullptr;
            sp.neighborRadiusM = static_cast<float>(d);
        }
        if (args.size() >= tail + 2) {
            if (!parseDouble(args[tail + 1], d))
                return nullptr;
            sp.separationRadiusM = static_cast<float>(d);
        }
        if (args.size() >= tail + 3) {
            if (!parseDouble(args[tail + 2], d))
                return nullptr;
            sp.cruiseThrottle = static_cast<float>(d);
        }
        if (behavior == "swarm")
            return std::make_unique<SwarmController>(*entityManager, point, sp);
        return std::make_unique<SwarmController>(*entityManager, anchorId, sp);
    }

    // -----------------------------------------------------------------------
    // wingman <anchorIdx> <command> [slotIndex=0]
    //
    // Attach one of the six scripted wingman commands (WingmanCommand.h) directly, without a
    // formation roster. This is the game-master / debugging path and the same code the network order
    // path drives, so a console order and a radio order cannot behave differently.
    //
    // NOTE: `attack_my_target` has no designated target on this path (designation comes from the
    // commander's boresight, which the console does not have), so it degrades to holding station.
    // Use `pursuit`/`lead` to point an AI at a specific entity from the console.
    // -----------------------------------------------------------------------
    if (behavior == "wingman") {
        if (args.size() < 2 || !entityManager)
            return nullptr;
        uint32_t idx{};
        if (!parseUint32(args[0], idx))
            return nullptr;
        fl::EntityId anchorId = findEntityById(idx);
        if (!anchorId.valid())
            return nullptr;

        const std::optional<WingmanCommand> cmd = parseWingmanCommand(args[1]);
        if (!cmd)
            return nullptr;

        WingmanParams wp{};
        if (args.size() >= 3 && !parseUint32(args[2], wp.slotIndex))
            return nullptr;

        // Home = the anchor's current position, so an RTB from the console goes somewhere sane.
        if (const fl::EntityState* as = entityManager->get(anchorId)) {
            wp.homePoint = glm::dvec3(as->transform.pos[0], as->transform.pos[1], as->transform.pos[2]);
        }
        return makeWingmanController(*entityManager, anchorId, *cmd, fl::EntityId{}, wp);
    }

    return nullptr;
}

} // namespace fl::ai
