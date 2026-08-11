// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sensor/SensorDef.h" // sensor::SensorType (the seeker channel)
#include "weapon/WeaponDef.h" // CountermeasureSusceptibility

#include <glm/vec3.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fl {

// Which seeker channel an expendable defeats (#529). Chaff blooms a cloud of radar reflectors that
// pull a RADAR seeker; a flare burns hot to pull an INFRARED one. The two are not interchangeable —
// popping flares against a radar missile does nothing, which is the whole tactical point of knowing
// what is shooting at you (the RWR, #526).
enum class DecoyKind : uint8_t { Chaff = 0, Flare = 1 };

// One expendable in the air. A decoy is dropped at the aircraft and then LAGS it — it inherits the
// launch velocity but decays and falls, so a fresh flare sits right on the target and an old one is a
// kilometre behind. That separation IS the model: a seeker is pulled while the decoy is close and
// recovers once it has left the target behind, which is why timing a break turn with the pop matters.
struct Decoy {
    uint32_t dispenserIdx{0};
    DecoyKind kind{DecoyKind::Flare};
    glm::dvec3 pos{};
    glm::vec3 vel{};
    uint64_t expireTick{0};
};

// Server-side expendable countermeasures (#529): the dispenser magazines and the decoys in the air,
// plus the seduction verdict a missile seeker asks of it. Sim-thread only (the weapons phase is
// serial), so no locking; deterministic (the seduction die is a seeded hash, never rand) so a replay
// on any machine breaks the same locks.
class CountermeasureSystem {
  public:
    // A fresh flare/chaff cloud is effective while within this radius of the target it is meant to
    // decoy. Beyond it the seeker has left the decoy behind and reacquires.
    static constexpr double kEffectRadiusM = 300.0;
    // Each channel's expendables burn/bloom for this long, then stop decoying.
    static constexpr float kLifetimeS = 4.0f;
    // The decoy sheds its inherited speed at this fraction per second and falls under a light gravity
    // proxy — enough to lag the aircraft without pretending to be a full ballistic body.
    static constexpr float kVelDecayPerS = 0.6f;
    static constexpr float kFallAccelMps2 = 4.0f;

    // Register / drop a dispenser's magazine (chaff + flare counts). Called on spawn; a count of 0 =
    // that channel has no expendables. removeDispenser on despawn (its airborne decoys age out).
    void registerDispenser(uint32_t entityIdx, uint16_t chaffCount, uint16_t flareCount);
    void removeDispenser(uint32_t entityIdx);

    // Pop one chaff + one flare from `entityIdx`'s magazines at `pos` with inherited `vel` (a combined
    // dispense — real programs mix both). Returns true if ANYTHING was released (either magazine had a
    // round). No-op / false when both magazines are empty or the entity has no dispenser.
    bool dispense(uint32_t entityIdx, const glm::dvec3& pos, const glm::vec3& vel, uint64_t tick);

    // Age the airborne decoys: integrate the lag/fall and drop expired ones. Serial, once per tick.
    void onTick(double simDt, uint64_t tick);

    // Would an expendable seduce a seeker this check (#529)? True when a matching-channel decoy is
    // within kEffectRadiusM of `targetPos` AND the deterministic die comes up under the missile's
    // susceptibility to that channel. `channel` is the seeker's sensor channel (Radar ⇒ chaff,
    // Ir ⇒ flare; anything else is undecoyable and returns false). Const + deterministic.
    [[nodiscard]] bool seduces(uint32_t missileIdx, const glm::dvec3& targetPos, sensor::SensorType channel,
                               const CountermeasureSusceptibility& susc, uint64_t tick) const;

    // Telemetry / tests.
    [[nodiscard]] std::size_t liveDecoyCount() const noexcept {
        return m_decoys.size();
    }
    [[nodiscard]] uint16_t chaffRemaining(uint32_t entityIdx) const;
    [[nodiscard]] uint16_t flareRemaining(uint32_t entityIdx) const;

  private:
    struct Magazine {
        uint16_t chaff{0};
        uint16_t flare{0};
    };
    std::unordered_map<uint32_t, Magazine> m_dispensers;
    std::vector<Decoy> m_decoys;
};

} // namespace fl
