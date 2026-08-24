// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/CountermeasureSystem.h"

#include "sensor/Detection.h" // sensor::rollPasses -- the ONE 24-bit threshold roll (#1265)

#include <glm/geometric.hpp> // glm::dot

namespace fl {

namespace {

// Deterministic seduction die for one (missile, tick) check, so a broken lock is identical across
// worker counts, platforms and replays.
//
// The HASH is this system's own -- it is seeded from the missile and the tick, which is what makes
// the roll reproducible here. The THRESHOLD TEST is sensor::rollPasses: the 24-bit integer compare
// on the hash's high bits, with the p<=0 / p>=1 early-outs. That half was written out a second time
// operation for operation (#1265), and two copies of a probability rule are two chances for a decoy
// to become slightly more or less effective than the sensor model it is fighting.
[[nodiscard]] bool rollSeduce(uint32_t missileIdx, uint64_t tick, float fraction) noexcept {
    uint32_t rng = missileIdx * 0x9E3779B1u + static_cast<uint32_t>(tick) * 0xC2B2AE3Du +
                   static_cast<uint32_t>(tick >> 32) * 0x27D4EB2Fu + 0x165667B1u;
    rng = rng * 1664525u + 1013904223u;
    rng ^= rng >> 15;
    rng = rng * 1664525u + 1013904223u;
    return sensor::rollPasses(rng, fraction);
}

[[nodiscard]] DecoyKind kindForChannel(sensor::SensorType channel, bool& decoyable) noexcept {
    switch (channel) {
    case sensor::SensorType::Radar:
        decoyable = true;
        return DecoyKind::Chaff;
    case sensor::SensorType::Ir:
        decoyable = true;
        return DecoyKind::Flare;
    default:
        decoyable = false; // visual / laser seekers are not decoyed by chaff or flare
        return DecoyKind::Flare;
    }
}

} // namespace

void CountermeasureSystem::registerDispenser(uint32_t entityIdx, uint16_t chaffCount, uint16_t flareCount) {
    m_dispensers[entityIdx] = Magazine{chaffCount, flareCount};
}

void CountermeasureSystem::removeDispenser(uint32_t entityIdx) {
    m_dispensers.erase(entityIdx);
    // Airborne decoys from this dispenser are left to age out — they are already in the air.
}

bool CountermeasureSystem::dispense(uint32_t entityIdx, const glm::dvec3& pos, const glm::vec3& vel, uint64_t tick) {
    auto it = m_dispensers.find(entityIdx);
    if (it == m_dispensers.end())
        return false;
    Magazine& mag = it->second;

    bool released = false;
    const uint64_t expire = tick + static_cast<uint64_t>(std::lround(kLifetimeS * 60.0)); // ~60 Hz ticks
    if (mag.chaff > 0) {
        --mag.chaff;
        m_decoys.push_back(Decoy{entityIdx, DecoyKind::Chaff, pos, vel, expire});
        released = true;
    }
    if (mag.flare > 0) {
        --mag.flare;
        m_decoys.push_back(Decoy{entityIdx, DecoyKind::Flare, pos, vel, expire});
        released = true;
    }
    return released;
}

void CountermeasureSystem::onTick(double simDt, uint64_t tick) {
    const float dt = static_cast<float>(simDt);
    for (Decoy& d : m_decoys) {
        // Lag the aircraft: shed inherited speed and fall under a light gravity proxy.
        d.pos += glm::dvec3(d.vel) * static_cast<double>(dt);
        d.vel -= d.vel * (kVelDecayPerS * dt);
        d.vel.y -= kFallAccelMps2 * dt;
    }
    std::erase_if(m_decoys, [tick](const Decoy& d) { return tick >= d.expireTick; });
}

bool CountermeasureSystem::seduces(uint32_t missileIdx, const glm::dvec3& targetPos, sensor::SensorType channel,
                                   const CountermeasureSusceptibility& susc, uint64_t tick) const {
    bool decoyable = false;
    const DecoyKind kind = kindForChannel(channel, decoyable);
    if (!decoyable)
        return false;
    const float fraction = (kind == DecoyKind::Chaff) ? susc.chaff : susc.flare;
    if (fraction <= 0.f)
        return false; // this seeker head is immune to that expendable

    // Is a matching-channel decoy still close to the target?
    const double r2 = kEffectRadiusM * kEffectRadiusM;
    bool nearby = false;
    for (const Decoy& d : m_decoys) {
        if (d.kind != kind)
            continue;
        const glm::dvec3 delta = d.pos - targetPos;
        if (glm::dot(delta, delta) <= r2) {
            nearby = true;
            break;
        }
    }
    if (!nearby)
        return false;

    return rollSeduce(missileIdx, tick, fraction);
}

uint16_t CountermeasureSystem::chaffRemaining(uint32_t entityIdx) const {
    auto it = m_dispensers.find(entityIdx);
    return it != m_dispensers.end() ? it->second.chaff : 0u;
}

uint16_t CountermeasureSystem::flareRemaining(uint32_t entityIdx) const {
    auto it = m_dispensers.find(entityIdx);
    return it != m_dispensers.end() ? it->second.flare : 0u;
}

} // namespace fl
