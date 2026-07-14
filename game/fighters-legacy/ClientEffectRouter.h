// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/GameProtocol.h"
#include "render/ParticleSystem.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace fl {

// One decoded SnapshotEffects TLV record (#625).
struct EffectEvent {
    uint8_t type{0};        // EffectType ordinal; unknown values are no-ops (forward compatible)
    uint8_t weaponClass{0}; // WeaponType ordinal
    uint32_t srcIdx{0xFFFFFFFFu};
    uint32_t tgtIdx{0xFFFFFFFFu};
    glm::dvec3 pos{};
};

// Routes cosmetic weapon effects to the particle system (#625). Audio and haptics join in #631 —
// this class is their seam, so the mapping lives in exactly one place. Deliberately game-layer:
// the engine stays ignorant of the presentation policy.
//
// Effects ride the unreliable snapshot; a dropped packet loses cosmetics, never state, so this
// router has no memory of what it missed — it holds only the short-lived emitters still burning.
class ClientEffectRouter {
  public:
    void onEffect(const EffectEvent& ev);

    // Per-frame one-shot emitters: live effects emit until their ttl expires. The returned span is
    // valid until the next call. `ps` supplies the preset definitions.
    [[nodiscard]] std::span<const ParticleEmitterState> buildEmitters(ParticleSystem& ps, float dt);

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
};

// Decode a SnapshotEffects TLV payload (kEffectRecordBytes per record, unaligned little-endian)
// and feed each record to the router. Trailing partial records are ignored (fail closed).
void routeEffectsTlv(ClientEffectRouter& router, const uint8_t* data, std::size_t size);

} // namespace fl
