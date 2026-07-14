// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h" // PayloadEffect

namespace fl {

struct EntityDef;
class ILogger;
class WeaponRegistry;

// Sums an entity's DEFAULT loadout into the single value the flight integrator understands (#812).
//
// This is the ONLY coupling between what an aircraft carries and how it flies, and until now it did
// not exist: EntityDef::hardpoints parsed and was read by nobody, and PayloadEffect was hard-coded
// to {} at both integrator call sites. A loadout cost the airframe exactly zero mass and zero drag.
//
// TWO RULES WORTH KNOWING BEFORE YOU EDIT THIS:
//
//  1. HardpointType::Fuel has NO WeaponType counterpart. The two enums are deliberately parallel --
//     Missile, Bomb, Rocket, Gun, Pod line up one-for-one -- and then HardpointType has Fuel, which
//     is a store that occupies a station without being a weapon. A drop tank is not in the weapon
//     registry and never will be. It is skipped explicitly, and silently: it is not an error.
//
//  2. An unknown or not-`allowed` store id is an ERROR-LOGGED SKIP, not a spawn failure. The
//     aircraft still flies -- it just flies clean, and the log says which pylon lied. Refusing to
//     spawn an aircraft because someone typo'd one weapon id is a worse failure than flying it
//     without that store and shouting about it.
//
// Main-thread-only; call once per entity type at load, and cache the result.
[[nodiscard]] PayloadEffect defaultPayload(const EntityDef& def, const WeaponRegistry& weapons, ILogger& log);

} // namespace fl
