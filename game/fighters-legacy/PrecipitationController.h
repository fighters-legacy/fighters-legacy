// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/ParticleSystem.h"

#include <array>
#include <span>

// Builds the 3×3 grid of precipitation particle emitters each frame based on
// the current weather state and camera position.
//
// Returns an empty span when cloud coverage is below the precipitation threshold.
// The returned span is valid until the next call to build().
class PrecipitationController {
  public:
    [[nodiscard]] std::span<const ParticleEmitterState> build(const EnvironmentState& env, const CameraView& cam,
                                                              bool isSnow, fl::ParticleSystem& ps) {
        if (env.cloudCoverage < kCloudThreshold)
            return {};

        const bool isStorm = env.cloudCoverage >= kStormThreshold;
        const char* presetName = isStorm ? (isSnow ? "storm_snow" : "storm_rain") : (isSnow ? "snow" : "rain");

        const auto preset = ps.getPreset(presetName);
        if (!preset)
            return {};

        const glm::vec3 camF = glm::vec3(cam.worldOrigin);
        const float influence = isStorm ? (isSnow ? 0.55f : 0.25f) : (isSnow ? 0.35f : 0.10f);
        const glm::vec3 windDir = glm::normalize(glm::vec3{env.windX * influence, -1.0f, env.windZ * influence});

        int idx = 0;
        for (int gx = -1; gx <= 1; ++gx) {
            for (int gz = -1; gz <= 1; ++gz, ++idx) {
                ParticleEmitterState& e = m_buf[idx];
                e = ParticleEmitterState{};
                e.position = camF + glm::vec3(gx * 40.0f, 60.0f, gz * 40.0f);
                e.effectName = presetName;
                e.intensity = 1.0f;
                e.spawnRate = preset->spawnRate;
                e.particleLifetime = preset->particleLifetime;
                e.initialSpeed = preset->initialSpeed;
                e.colorStart = preset->colorStart;
                e.colorEnd = preset->colorEnd;
                e.sizeStart = preset->sizeStart;
                e.sizeEnd = preset->sizeEnd;
                e.additive = preset->additive;
                e.emitDirection = windDir;
                e.coneHalfAngleDeg = preset->coneHalfAngleDeg;
            }
        }
        return {m_buf.data(), 9};
    }

  private:
    static constexpr float kCloudThreshold = 0.75f;
    static constexpr float kStormThreshold = 0.90f;

    std::array<ParticleEmitterState, 9> m_buf{};
};
