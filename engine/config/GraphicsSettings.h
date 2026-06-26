// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

enum class VsyncMode : uint8_t { Off, On, Adaptive };
enum class FrameRateCap : uint8_t { Off, Cap30, Cap60, Cap120, Cap144, Cap240 };
enum class QualityLevel : uint8_t { Low, Medium, High, Ultra };
enum class DrawDistance : uint8_t { Low, Medium, High, Ultra };
enum class UiScale : uint8_t { Scale75, Scale100, Scale125, Scale150 };

// AA mode selector — ordinals must stay in sync with RendererAAMode in platform/RenderTypes.h.
// MSAA removed in favour of TAA (superseded #375).
enum class AntiAliasingMode : uint8_t { Off, FXAA, TAA };

// Shadow quality — ordinals must stay in sync with RendererShadowQuality in platform/RenderTypes.h.
enum class ShadowQuality : uint8_t { Off, Low, Medium, High, Ultra };

// Particle density — ordinals must stay in sync with RendererParticleDensity in platform/RenderTypes.h.
enum class ParticleDensity : uint8_t { Low, Medium, High, Ultra };

// Ambient occlusion (GTAO) quality — ordinals must stay in sync with RendererAOMode in
// platform/RenderTypes.h.
enum class AmbientOcclusion : uint8_t { Off, Low, High };

// Sky scattering model — ordinals must stay in sync with RendererSkyQuality in platform/RenderTypes.h.
enum class SkyQuality : uint8_t { Procedural, LUT };

struct GraphicsSettings {
    int resolutionWidth = 0;  // 0 = native
    int resolutionHeight = 0; // 0 = native
    VsyncMode vsync = VsyncMode::On;
    FrameRateCap frameRateCap = FrameRateCap::Off;
    QualityLevel qualityPreset = QualityLevel::High;
    DrawDistance drawDistance = DrawDistance::High;
    AntiAliasingMode aaMode = AntiAliasingMode::TAA;
    ShadowQuality shadowQuality = ShadowQuality::High;
    ParticleDensity particleDensity = ParticleDensity::High;
    AmbientOcclusion ambientOcclusion = AmbientOcclusion::High;
    SkyQuality skyQuality = SkyQuality::LUT;
    UiScale uiScale = UiScale::Scale100;
    int cockpitFov = 90; // degrees, clamped [60, 120]
};

} // namespace fl
