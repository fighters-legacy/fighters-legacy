// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fl {

// CPU-side description of one particle effect type.
// Parameters are copied into ParticleEmitterState by emit() so the renderer
// receives all GPU-relevant data without its own preset table.
struct ParticlePreset {
    float spawnRate{50.0f};                 // particles/second
    float particleLifetime{2.0f};           // seconds
    float initialSpeed{5.0f};               // m/s, randomised within cone of coneHalfAngleDeg around emitDirection
    glm::vec3 colorStart{1.0f, 0.5f, 0.1f}; // orange-yellow (birth)
    glm::vec3 colorEnd{0.3f, 0.3f, 0.3f};   // gray smoke (death)
    float sizeStart{0.5f};                  // world-space metres
    float sizeEnd{2.0f};
    bool additive{true};                       // true=additive (fire/explosion), false=alpha (smoke)
    glm::vec3 emitDirection{0.0f, 1.0f, 0.0f}; // normalised; cone centred on this axis
    float coneHalfAngleDeg{90.0f};             // emission cone half-angle; 90=hemisphere (default)
};

// Manages per-frame particle emitter emission.
//
// Usage pattern each frame:
//   particleSystem.reset();
//   // ... for each active damage source:
//   particleSystem.emit("explosion", entityPos);
//   // Pass to FrameScene:
//   sceneRenderer.renderFrame(alpha, camera, env, particleSystem.emitters());
class ParticleSystem {
  public:
    // Register a named preset. Subsequent emit() calls referencing this name
    // resolve to these parameters. Overwriting an existing name is allowed.
    void registerPreset(std::string_view name, ParticlePreset preset);

    // Emit particles at worldPosition for the named preset this frame.
    // Silently ignored if the preset name is not registered or name is nullptr.
    void emit(const char* presetName, glm::vec3 worldPosition, float intensity = 1.0f);

    // Returns all emitters accumulated since the last reset().
    // The span is valid until the next reset() or emit() call.
    [[nodiscard]] std::span<const ParticleEmitterState> emitters() const noexcept;

    // Returns the registered preset for name, or nullopt if not found.
    [[nodiscard]] std::optional<ParticlePreset> getPreset(const char* name) const noexcept;

    // Clear the per-frame emitter list. Call once per frame before any emit() calls.
    void reset() noexcept;

  private:
    std::unordered_map<std::string, ParticlePreset> m_presets;
    std::vector<ParticleEmitterState> m_emitters;
};

} // namespace fl
