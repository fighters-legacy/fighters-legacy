// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// ── Crew seats & turret mounts (#966/#968) ───────────────────────────────────
//
// An aircraft is a set of crew SEATS, one per authored [[crew]] table. seats[0] is the pilot
// and IS today's single-controller path; an EntityDef with no [[crew]] section is the implicit
// 1-seat case (see the runtime crew frame), so every existing aircraft is a valid crewed
// aircraft with zero content churn.
//
// "Station" already means a weapon hardpoint (Hardpoint::slot, FireRequest::station). Crew
// positions are SEATS everywhere, never "stations".
//
// CAPABILITY PARTITION, NOT A ROLE ENUM. A seat's `role` is a display string ("pilot",
// "tail-gunner"); the engine and the wire see only a CrewCapabilityMask. This mirrors the #944
// roles-as-data decision and the "`allowed` IS the contract" hardpoint precedent: the machine
// contract is the capability bits, and the invariant that makes a masked merge conflict-free is
// ONE OWNER PER CONTROL CHANNEL (validateCrewPartition below).

// A single crew capability bit. The mask is a uint16 (CrewCapabilityMask).
enum class CrewCapability : uint16_t {
    None = 0,
    Fly = 1u << 0,             // owns the flight controls (elevator/aileron/rudder/throttle/AB/etc.)
    Fire = 1u << 1,            // fires a bound set of hardpoint stations (direct and/or turret-mounted)
    Radar = 1u << 2,           // owns radar mode + designation (#526/#587)
    Countermeasures = 1u << 3, // owns chaff/flare dispense + ECM (#529)
    Command = 1u << 4,         // the Epic O / #591 seam — reserved, no runtime consumer yet
};

using CrewCapabilityMask = uint16_t;

[[nodiscard]] constexpr bool hasCapability(CrewCapabilityMask mask, CrewCapability cap) noexcept {
    return (mask & static_cast<uint16_t>(cap)) != 0u;
}

[[nodiscard]] constexpr CrewCapabilityMask withCapability(CrewCapabilityMask mask, CrewCapability cap) noexcept {
    return static_cast<CrewCapabilityMask>(mask | static_cast<uint16_t>(cap));
}

// Every capability bit, in wire/display order — the single source of truth iterated by the parser,
// the manual, and validation. (Excludes None.)
[[nodiscard]] const std::vector<CrewCapability>& allCrewCapabilities();

// Token <-> capability. Tokens are the authored strings: "fly", "fire", "radar",
// "countermeasures", "command". nullopt = unknown token (the caller decides how to report it).
[[nodiscard]] std::optional<CrewCapability> parseCrewCapability(std::string_view token) noexcept;
[[nodiscard]] std::string_view crewCapabilityName(CrewCapability cap) noexcept;

// Authored default occupancy for a seat. Human is a RUNTIME state (a peer claims a seat), never
// authored — so authoring is two-state and the wire/runtime is three-state {Bot | Human | Empty}.
enum class SeatOccupancyDefault : uint8_t {
    Bot,  // spawn a bot in this seat (bot spec below); the common case
    Empty // spawn nobody — a human-joinable-only seat (owner decision: unmanned seats are legal)
};

// One authored crew position.
struct SeatDef {
    std::string role;                  // display string, e.g. "pilot" / "tail-gunner"; non-empty
    CrewCapabilityMask capabilities{}; // the machine contract; non-zero
    std::vector<int> stations;         // hardpoint slots this seat fires DIRECTLY (Fire seats)
    std::string turret;                // turret id this seat aims (its stations are fired too); empty = none
    float eyepoint[3]{0.f, 0.f, 0.f};  // seat/eyepoint position, body frame (metres) — per-seat cockpit camera
    SeatOccupancyDefault defaultOccupancy{SeatOccupancyDefault::Bot};
    std::string botSpec;      // bot behavior spec (factory behavior / "builtin:*" / "lua:*"); empty = engine default
    float defaultSkill{0.5f}; // [0,1] per-instance skill baseline (a mission range may override)

    // Crew-seat damage (#978, builds on #675). A seat with damageHp > 0 is an independent damage pool
    // routed by the subsystem weighted/quadrant pick; when its pool is exhausted the seat is KNOCKED
    // OUT and goes silent (its channels stop — a dead Fly seat = no pilot input; a dead gunner = the
    // turret stops), bot or human alike. damageHp == 0 (the default) = a non-damageable seat, which
    // preserves the #675 fallback (no crew-seat entries ⇒ behavior unchanged).
    float damageHp{0.f};  // seat HP pool; 0 = not damageable
    float hitWeight{1.f}; // relative hit-bias weight in the subsystem pick (> 0)
};

// One authored turret mount — a weapon station's aiming frame, independent of the airframe nose.
// The runtime slew servo (stepTurret) and the directional launch vector are #970; this is schema.
struct TurretDef {
    std::string id;                           // unique within the entity; referenced by SeatDef::turret
    float mountPos[3]{0.f, 0.f, 0.f};         // mount origin, body frame (metres)
    float mountOrient[4]{0.f, 0.f, 0.f, 1.f}; // mount rest orientation, body-frame quat (x,y,z,w); default = nose
    float azMinDeg{-180.f};                   // traverse limits about the mount's up axis
    float azMaxDeg{180.f};
    float elMinDeg{-5.f}; // elevation limits about the mount's right axis
    float elMaxDeg{85.f};
    float slewRateDegS{60.f};  // servo slew rate (deg/s); > 0
    std::vector<int> stations; // hardpoint slots physically mounted on this turret
};

// The one-owner-per-channel invariant (#966). Returns "" when the partition is valid, else a
// human-readable message naming the first violation. Pure and dependency-light (takes the plain
// declared hardpoint slots, not Hardpoint, so this header does not pull EntityDef) — reused by the
// parser (throw) and by validate-entity (which surfaces the throw with the file name).
//
// Enforced (crew non-empty):
//   * exactly one Fly seat;
//   * Radar / Countermeasures / Command each on at most one seat;
//   * each hardpoint slot fired by at most one seat (direct stations + turret-mounted);
//   * a seat that binds stations or a turret has the Fire capability, and a Fire seat fires
//     something;
//   * every referenced slot / turret id resolves; turret ids are unique; az/el limits and slew
//     are sane; each turret is aimed by at most one seat.
[[nodiscard]] std::string validateCrewPartition(const std::vector<SeatDef>& crew, const std::vector<TurretDef>& turrets,
                                                const std::vector<int>& hardpointSlots);

} // namespace fl
