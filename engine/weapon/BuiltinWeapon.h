// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "weapon/WeaponDef.h"

namespace fl {

// Compiled-in weapon for the zero-content-pack sandbox, mirroring BuiltinFlightModel: the engine is
// content-agnostic, but it must still be able to arm something with no pack mounted. Deliberately
// bland — a generic short-range IR missile, not a real one.
struct BuiltinWeapon {
    [[nodiscard]] static const WeaponDef& get() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:test-missile";
            d.name = "Test Missile";
            d.type = WeaponType::Missile;
            d.category = WeaponCategory::AirToAir;

            SeekerDef s;
            s.type = SeekerType::Infrared;
            s.fovDeg = 60.f;
            s.acquisitionRangeM = 9260.f; // 5 nm
            s.fireAndForget = true;
            d.seeker = s;

            d.performance.maxRangeM = 18520.f; // 10 nm
            d.performance.minRangeM = 500.f;
            d.performance.maxSpeedMps = 771.7f; // ~Mach 2.3 at sea level
            d.performance.motorBurnTimeS = 5.f;
            d.performance.maxG = 30.f;

            d.warhead.blastRadiusM = 10.f;
            d.warhead.damage = 100.f;

            d.countermeasures.flare = 0.5f;

            d.load.massKg = 85.f;
            d.load.dragFactor = 0.006f;
            return d;
        }();
        return w;
    }
};

} // namespace fl
