// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "weapon/WeaponDef.h"

namespace fl {

// Compiled-in weapons for the zero-content-pack sandbox (#440), mirroring BuiltinFlightModel /
// BuiltinSensors: the engine is content-agnostic, but it must still be able to arm something with
// no pack mounted — the whole fire path (#625) has to be provable from a bare checkout.
//
// Deliberately bland, plausible-class values — an M61-class cannon, an AIM-9-class IR missile and
// an AMRAAM-class active-radar missile, never a specific real weapon. The "builtin:" namespace
// cannot collide with a content pack's; packs override the sandbox by simply existing (the debug
// entity carries builtins, pack aircraft carry pack stores).
struct BuiltinWeapon {
    [[nodiscard]] static const WeaponDef& cannon() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:cannon";
            d.name = "20mm Cannon";
            d.type = WeaponType::Gun;
            d.category = WeaponCategory::AirToAir;

            d.performance.maxRangeM = 1200.f; // effective air-to-air gun range
            d.performance.rateOfFireRpm = 6000.f;

            d.warhead.blastRadiusM = 1.f;
            d.warhead.damage = 9.f; // a 20 mm HEI hit vs a 100 hp fighter: ~a dozen hits kill

            d.load.massKg = 250.f; // gun + full drum
            d.load.dragFactor = 0.f;
            d.load.rounds = 640;
            return d;
        }();
        return w;
    }

    [[nodiscard]] static const WeaponDef& irMissile() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:ir-missile";
            d.name = "IR Missile";
            d.type = WeaponType::Missile;
            d.category = WeaponCategory::AirToAir;

            SeekerDef s;
            s.type = SeekerType::Infrared;
            s.sensorId = "builtin:ir-seeker"; // BuiltinSensors::irSeeker() — the one vocabulary
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
            d.load.rounds = 1;
            return d;
        }();
        return w;
    }

    [[nodiscard]] static const WeaponDef& radarMissile() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:radar-missile";
            d.name = "Radar Missile";
            d.type = WeaponType::Missile;
            d.category = WeaponCategory::AirToAir;

            SeekerDef s;
            s.type = SeekerType::ActiveRadar;
            s.sensorId = "builtin:radar-seeker"; // BuiltinSensors::radarSeeker()
            s.fireAndForget = true;              // ARH: own radar takes over at pitbull (#628)
            s.pitbullRangeM = 12000.f;           // ~6.5 nm to go: seeker goes active (and EMITS)
            s.loftBiasDeg = 20.f;                // climb bias while far out — thin air is range
            s.loftRangeM = 27780.f;              // 15 nm: below this, pure PN
            d.seeker = s;

            d.performance.maxRangeM = 55560.f; // 30 nm
            d.performance.minRangeM = 900.f;
            d.performance.maxSpeedMps = 1372.f; // ~Mach 4 at sea level
            d.performance.motorBurnTimeS = 8.f;
            d.performance.maxG = 30.f;

            d.warhead.blastRadiusM = 15.f;
            d.warhead.damage = 100.f;

            d.countermeasures.chaff = 0.5f;

            d.load.massKg = 152.f;
            d.load.dragFactor = 0.004f;
            d.load.rounds = 1;
            return d;
        }();
        return w;
    }
};

} // namespace fl
