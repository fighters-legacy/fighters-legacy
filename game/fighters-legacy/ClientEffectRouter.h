// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config/AudioSettings.h"
#include "net/GameProtocol.h"
#include "render/ParticleSystem.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace fl {

class SfxManager;
class HapticController;

// One decoded SnapshotEffects TLV record (#625).
struct EffectEvent {
    uint8_t type{0};        // EffectType ordinal; unknown values are no-ops (forward compatible)
    uint8_t weaponClass{0}; // WeaponType ordinal
    uint32_t srcIdx{0xFFFFFFFFu};
    uint32_t tgtIdx{0xFFFFFFFFu};
    glm::dvec3 pos{};
};

// Routes cosmetic weapon effects to the particle system, SFX and haptics (#625/#631) — the ONE
// place the wire's effect vocabulary becomes presentation, so the mapping cannot drift. Deliberately
// game-layer: the engine stays ignorant of the presentation policy.
//
// Effects ride the unreliable snapshot; a dropped packet loses cosmetics, never state, so this
// router has no memory of what it missed — it holds only the short-lived emitters still burning.
class ClientEffectRouter {
  public:
    void onEffect(const EffectEvent& ev);

    // Per-frame one-shot emitters: live effects emit until their ttl expires. The returned span is
    // valid until the next call. `ps` supplies the preset definitions.
    [[nodiscard]] std::span<const ParticleEmitterState> buildEmitters(ParticleSystem& ps, float dt);

    // Optional presentation collaborators (#631). All null-safe: with none wired the router is
    // exactly the particle-only #625 router (which is why test_effect_router needs no audio).
    void setSfx(SfxManager* sfx, const AudioSettings* settings) noexcept {
        m_sfx = sfx;
        m_audioSettings = settings;
    }
    void setHaptics(HapticController* haptics) noexcept {
        m_haptics = haptics;
    }
    // The player's own entity index — own-ship launches/releases drive haptics, and own gunfire
    // plays head-relative (your own gun is loud regardless of the camera). 0xFFFFFFFF = unknown.
    void setOwnEntity(uint32_t idx) noexcept {
        m_ownEntityIdx = idx;
    }
    // The camera world origin (updated each frame): SFX are spatialised relative to it, matching the
    // renderer's camera-relative invariant.
    void setCameraOrigin(const glm::dvec3& origin) noexcept {
        m_cameraOrigin = origin;
    }

    void reset() noexcept {
        m_count = 0;
    }

    static constexpr int kMaxActive = 32;

  private:
    struct ActiveEffect {
        const char* preset{nullptr};
        glm::dvec3 pos{};
        float ttl{0.f};
        float intensity{1.f};
    };
    ActiveEffect m_active[kMaxActive]{};
    int m_count{0};
    std::vector<ParticleEmitterState> m_scratch;

    SfxManager* m_sfx{nullptr};
    const AudioSettings* m_audioSettings{nullptr};
    HapticController* m_haptics{nullptr};
    uint32_t m_ownEntityIdx{0xFFFFFFFFu};
    glm::dvec3 m_cameraOrigin{};
};

// Decode a SnapshotEffects TLV payload (kEffectRecordBytes per record, unaligned little-endian)
// and feed each record to the router. Trailing partial records are ignored (fail closed).
void routeEffectsTlv(ClientEffectRouter& router, const uint8_t* data, std::size_t size);

} // namespace fl
