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

    // An SARH missile (#862) — SemiActiveRadar: it flies the whole way on energy the SHOOTER's radar
    // reflects off the target (SupportQuery / illumination path, ProjectileSystem), never radiates
    // itself, and requires continuous support (fire-and-forget = false, no pitbull). Distinct from the
    // fire-and-forget IR and the go-active ARH: this is the path the RWR seam reads as "being painted".
    [[nodiscard]] static const WeaponDef& sarhMissile() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:sarh-missile";
            d.name = "SARH Missile";
            d.type = WeaponType::Missile;
            d.category = WeaponCategory::AirToAir;

            SeekerDef s;
            s.type = SeekerType::SemiActiveRadar;
            s.sensorId = "builtin:sarh-seeker"; // BuiltinSensors::sarhSeeker() — a passive receiver
            s.fireAndForget = false;            // the launch platform must keep illuminating the target
            d.seeker = s;

            d.performance.maxRangeM = 37040.f; // 20 nm
            d.performance.minRangeM = 900.f;
            d.performance.maxSpeedMps = 1234.f; // ~Mach 3.6 at sea level
            d.performance.motorBurnTimeS = 7.f;
            d.performance.maxG = 25.f;

            d.warhead.blastRadiusM = 15.f;
            d.warhead.damage = 100.f;

            d.countermeasures.chaff = 0.5f;

            d.load.massKg = 195.f; // an AIM-7-class SARH is heavier than an ARH of similar reach
            d.load.dragFactor = 0.005f;
            d.load.rounds = 1;
            return d;
        }();
        return w;
    }

    // An unguided iron bomb (#862) — the ballistic bomb-ejection path (ProjectileSystem downward
    // ejection): no seeker, no motor, no manoeuvre, a big warhead. Air-to-ground.
    [[nodiscard]] static const WeaponDef& bomb() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:bomb";
            d.name = "Iron Bomb";
            d.type = WeaponType::Bomb;
            d.category = WeaponCategory::AirToGround;

            d.performance.maxRangeM = 0.f;      // ballistic — range is whatever the release energy buys
            d.performance.motorBurnTimeS = 0.f; // unpowered
            d.performance.maxG = 0.f;           // unguided

            d.warhead.blastRadiusM = 60.f; // a ~500 lb-class blast
            d.warhead.damage = 300.f;

            d.load.massKg = 240.f; // ~500 lb
            d.load.dragFactor = 0.010f;
            d.load.rounds = 1;
            return d;
        }();
        return w;
    }

    // An unguided rocket pod (#862) — the ripple-while-held rocket path with cepM dispersion
    // (ProjectileSystem). A pod holds many rockets; each is a small motor-boosted unguided round.
    [[nodiscard]] static const WeaponDef& rocketPod() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:rocket";
            d.name = "Rocket Pod";
            d.type = WeaponType::Rocket;
            d.category = WeaponCategory::AirToGround;

            d.performance.maxRangeM = 4000.f;
            d.performance.maxSpeedMps = 730.f; // ~Mach 2.1 at burnout
            d.performance.motorBurnTimeS = 1.1f;
            d.performance.maxG = 0.f;  // unguided
            d.performance.cepM = 12.f; // launch dispersion — a rocket is an area weapon

            d.warhead.blastRadiusM = 10.f;
            d.warhead.damage = 40.f;

            d.load.massKg = 220.f; // a loaded 19-round pod
            d.load.dragFactor = 0.009f;
            d.load.rounds = 19;
            return d;
        }();
        return w;
    }

    // A drop tank (#862) — the inert Fuel store. It mounts like any other store and costs
    // the airframe mass + drag; it is never fired, selected, or released (see Loadout.h / FireControl).
    // No seeker, no warhead, no performance — a tank is not a weapon, it is ballast that used to hold
    // fuel. WeaponType::Fuel makes it mountable in the same registry as everything else.
    [[nodiscard]] static const WeaponDef& dropTank() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:drop-tank";
            d.name = "Drop Tank";
            d.type = WeaponType::Fuel;
            d.category = WeaponCategory::AirToAir; // category is irrelevant for an inert store

            d.load.massKg = 1000.f; // a ~370-gal tank, full
            d.load.dragFactor = 0.008f;
            d.load.rounds = 0; // never fires
            return d;
        }();
        return w;
    }

    // A targeting/ECM pod (#862) — the inert Pod store, so every WeaponType has a mounting builtin. It
    // mounts like any other store and costs mass + drag, but like the drop tank it never
    // fires (a pod carries sensors or jammers, not ordnance). No seeker, no warhead, no performance.
    [[nodiscard]] static const WeaponDef& pod() {
        static const WeaponDef w = [] {
            WeaponDef d;
            d.id = "builtin:pod";
            d.name = "Sensor Pod";
            d.type = WeaponType::Pod;
            d.category = WeaponCategory::AirToGround; // irrelevant for an inert store

            d.load.massKg = 210.f; // a targeting-pod-class store
            d.load.dragFactor = 0.006f;
            d.load.rounds = 0; // never fires
            return d;
        }();
        return w;
    }
};

} // namespace fl
