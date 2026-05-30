// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/ParticleSystem.h"

namespace fl {

void ParticleSystem::registerPreset(std::string_view name, ParticlePreset preset) {
    m_presets[std::string(name)] = preset;
}

void ParticleSystem::emit(const char* presetName, glm::vec3 worldPosition, float intensity) {
    if (!presetName)
        return;
    auto it = m_presets.find(presetName);
    if (it == m_presets.end())
        return;

    const ParticlePreset& p = it->second;
    ParticleEmitterState state{};
    state.position = worldPosition;
    // Point at the map key, not the caller's pointer: map nodes don't move after
    // insertion (std::unordered_map reference stability), so c_str() is stable for
    // the lifetime of the ParticleSystem. This avoids a dangling pointer when the
    // caller passes a local std::string::c_str().
    state.effectName = it->first.c_str();
    state.intensity = intensity;
    state.spawnRate = p.spawnRate;
    state.particleLifetime = p.particleLifetime;
    state.initialSpeed = p.initialSpeed;
    state.colorStart = p.colorStart;
    state.colorEnd = p.colorEnd;
    state.sizeStart = p.sizeStart;
    state.sizeEnd = p.sizeEnd;
    state.additive = p.additive;
    m_emitters.push_back(state);
}

std::span<const ParticleEmitterState> ParticleSystem::emitters() const noexcept {
    return m_emitters;
}

void ParticleSystem::reset() noexcept {
    m_emitters.clear();
}

} // namespace fl
