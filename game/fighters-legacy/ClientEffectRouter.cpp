// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientEffectRouter.h"

#include "HapticController.h"
#include "audio/SfxManager.h"

#include <cstring>

namespace fl {

namespace {

// EffectType → presentation row. Code, not data: the keys are wire enums owned by the protocol —
// content already overrides the substance behind each preset name via registerPreset.
struct EffectRow {
    const char* preset;
    float ttl;
    float intensity;
};

// The SFX preset name for an EffectType — the same vocabulary SfxManager registers. Empty = silent.
const char* sfxPresetFor(uint8_t type) {
    switch (static_cast<EffectType>(type)) {
    case EffectType::WeaponFired:
        return "sfx.gunfire";
    case EffectType::MissileLaunch:
        return "sfx.launch";
    case EffectType::Impact:
        return "sfx.impact";
    case EffectType::Detonation:
    case EffectType::NuclearFlash:
        return "sfx.explosion";
    }
    return nullptr;
}

const EffectRow* rowFor(uint8_t type) {
    static constexpr EffectRow kWeaponFired{"muzzle_flash", 0.08f, 1.f};
    static constexpr EffectRow kMissileLaunch{"muzzle_flash", 0.3f, 1.5f};
    static constexpr EffectRow kImpact{"impact_sparks", 0.25f, 1.f};
    static constexpr EffectRow kDetonation{"explosion", 0.5f, 1.f};
    static constexpr EffectRow kNuclearFlash{"explosion", 2.5f,
                                             8.f}; // the full-screen flash joins with #631's cue pass
    switch (static_cast<EffectType>(type)) {
    case EffectType::WeaponFired:
        return &kWeaponFired;
    case EffectType::MissileLaunch:
        return &kMissileLaunch;
    case EffectType::Impact:
        return &kImpact;
    case EffectType::Detonation:
        return &kDetonation;
    case EffectType::NuclearFlash:
        return &kNuclearFlash;
    }
    return nullptr; // unknown types are no-ops — the vocabulary grows without breaking old clients
}

} // namespace

void ClientEffectRouter::onEffect(const EffectEvent& ev) {
    const bool ownShip = m_ownEntityIdx != 0xFFFFFFFFu && ev.srcIdx == m_ownEntityIdx;

    // Audio (#631): play the mapped SFX at the effect's world position, spatialised camera-relative.
    // Own gunfire plays AT the camera (head-relative) — your own gun is loud regardless of the
    // chase-cam distance — by placing it at the camera origin.
    if (m_sfx && m_audioSettings) {
        if (const char* preset = sfxPresetFor(ev.type)) {
            const glm::dvec3 at =
                (ownShip && ev.type == static_cast<uint8_t>(EffectType::WeaponFired)) ? m_cameraOrigin : ev.pos;
            m_sfx->play(preset, at, m_cameraOrigin, *m_audioSettings);
        }
    }

    // Haptics (#631): an own-ship store leaving the rails kicks the controller. Gun recoil is NOT
    // here — HapticController::update already drives it from the per-frame wasWeaponFired flag, and
    // firing one pulse per round at the rate of fire would double it. Missile-approach WARNING is
    // deliberately absent too: an inbound-missile cue the moment the server knows would leak the
    // launch through the controller (a wallhack); it waits for the RWR/#529 sensor work.
    if (m_haptics && ownShip && ev.type == static_cast<uint8_t>(EffectType::MissileLaunch))
        m_haptics->notifyOrdnanceRelease();

    const EffectRow* row = rowFor(ev.type);
    if (!row)
        return;

    // Ring: overwrite the oldest when full — a burst of effects degrades to the newest N, which is
    // the right failure mode for cosmetics.
    int slot = m_count < kMaxActive ? m_count++ : 0;
    if (m_count >= kMaxActive) {
        float minTtl = m_active[0].ttl;
        slot = 0;
        for (int i = 1; i < kMaxActive; ++i) {
            if (m_active[i].ttl < minTtl) {
                minTtl = m_active[i].ttl;
                slot = i;
            }
        }
    }
    m_active[slot] = {row->preset, ev.pos, row->ttl, row->intensity};
}

std::span<const ParticleEmitterState> ClientEffectRouter::buildEmitters(ParticleSystem& ps, float dt) {
    m_scratch.clear();
    for (int i = 0; i < m_count;) {
        ActiveEffect& e = m_active[i];
        e.ttl -= dt;
        if (e.ttl <= 0.f) {
            m_active[i] = m_active[--m_count]; // swap-erase
            continue;
        }
        if (auto preset = ps.getPreset(e.preset)) {
            ParticleEmitterState em{};
            em.position = glm::vec3(e.pos); // float world pos — the existing emitter contract
            em.effectName = e.preset;       // static literal from the row table: pointer-stable
            em.intensity = e.intensity;
            em.spawnRate = preset->spawnRate;
            em.particleLifetime = preset->particleLifetime;
            em.initialSpeed = preset->initialSpeed;
            em.colorStart = preset->colorStart;
            em.colorEnd = preset->colorEnd;
            em.sizeStart = preset->sizeStart;
            em.sizeEnd = preset->sizeEnd;
            em.additive = preset->additive;
            em.emitDirection = preset->emitDirection;
            em.coneHalfAngleDeg = preset->coneHalfAngleDeg;
            m_scratch.push_back(em);
        }
        ++i;
    }
    return m_scratch;
}

void routeEffectsTlv(ClientEffectRouter& router, const uint8_t* data, std::size_t size) {
    const std::size_t count = size / kEffectRecordBytes; // partial trailing record: dropped
    for (std::size_t i = 0; i < count; ++i) {
        const uint8_t* p = data + i * kEffectRecordBytes;
        EffectEvent ev;
        ev.type = p[0];
        ev.weaponClass = p[1];
        std::memcpy(&ev.srcIdx, p + 2, 4);
        std::memcpy(&ev.tgtIdx, p + 6, 4);
        float pos[3];
        std::memcpy(pos, p + 10, 12);
        ev.pos = {pos[0], pos[1], pos[2]};
        router.onEffect(ev);
    }
}

} // namespace fl
