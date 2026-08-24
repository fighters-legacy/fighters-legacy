// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <memory>
#include <span>
#include <string_view>

namespace fl {
class EntityManager;
} // namespace fl

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
//
// The parsing itself lives in AiControllerFactory.cpp behind a small positional-argument cursor
// (#1265). It was 700 lines of inline if-chain in this header, which meant every consumer of the
// factory recompiled the whole controller catalogue and re-instantiated the same parse code.
[[nodiscard]] std::unique_ptr<fl::IEntityController> createController(std::string_view behavior,
                                                                      std::span<std::string_view> args,
                                                                      const fl::EntityManager* entityManager = nullptr);

} // namespace fl::ai
