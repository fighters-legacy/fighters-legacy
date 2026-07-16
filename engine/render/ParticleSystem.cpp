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
    state.emitDirection = p.emitDirection;
    state.coneHalfAngleDeg = p.coneHalfAngleDeg;
    m_emitters.push_back(state);
}

std::optional<ParticlePreset> ParticleSystem::getPreset(const char* name) const noexcept {
    if (!name)
        return std::nullopt;
    auto it = m_presets.find(name);
    if (it == m_presets.end())
        return std::nullopt;
    return it->second;
}

std::span<const ParticleEmitterState> ParticleSystem::emitters() const noexcept {
    return m_emitters;
}

void ParticleSystem::reset() noexcept {
    m_emitters.clear();
}

void registerBuiltinParticlePresets(ParticleSystem& ps) {
    ps.registerPreset("explosion", {200.0f, 1.5f, 15.0f, {1.0f, 0.6f, 0.1f}, {0.4f, 0.2f, 0.1f}, 0.3f, 3.0f, true});
    ps.registerPreset("fire", {120.0f, 2.0f, 8.0f, {1.0f, 0.4f, 0.05f}, {0.6f, 0.1f, 0.0f}, 0.2f, 1.5f, true});
    ps.registerPreset("smoke", {60.0f, 4.0f, 3.0f, {0.4f, 0.4f, 0.4f}, {0.15f, 0.15f, 0.15f}, 0.5f, 3.0f, false});
    // Weapon effects (#625): short, bright, additive.
    ps.registerPreset("muzzle_flash",
                      {400.0f, 0.15f, 12.0f, {1.0f, 0.9f, 0.5f}, {1.0f, 0.5f, 0.1f}, 0.15f, 0.5f, true});
    ps.registerPreset("impact_sparks", {300.0f, 0.4f, 20.0f, {1.0f, 0.8f, 0.4f}, {0.8f, 0.3f, 0.1f}, 0.1f, 0.3f, true});
    ps.registerPreset("missile_smoke", {80.0f, 2.5f, 2.0f, {0.8f, 0.8f, 0.8f}, {0.4f, 0.4f, 0.4f}, 0.3f, 1.8f, false});
    ps.registerPreset(
        "rain",
        {100.0f, 1.5f, 40.0f, {0.5f, 0.6f, 0.8f}, {0.3f, 0.4f, 0.6f}, 0.05f, 0.05f, false, {0.0f, -1.0f, 0.0f}, 20.0f});
    ps.registerPreset(
        "storm_rain",
        {200.0f, 1.2f, 50.0f, {0.6f, 0.7f, 0.9f}, {0.3f, 0.4f, 0.6f}, 0.08f, 0.08f, false, {0.0f, -1.0f, 0.0f}, 20.0f});
    ps.registerPreset("snow", {200.0f,
                               6.0f,
                               2.0f,
                               {0.9f, 0.95f, 1.0f},
                               {0.85f, 0.90f, 1.0f},
                               0.15f,
                               0.10f,
                               false,
                               {0.0f, -1.0f, 0.0f},
                               80.0f});
    ps.registerPreset("storm_snow", {400.0f,
                                     6.0f,
                                     4.0f,
                                     {0.9f, 0.95f, 1.0f},
                                     {0.85f, 0.90f, 1.0f},
                                     0.12f,
                                     0.08f,
                                     false,
                                     {0.0f, -1.0f, 0.0f},
                                     80.0f});
}

} // namespace fl
