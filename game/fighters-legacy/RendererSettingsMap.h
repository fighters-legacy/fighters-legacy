// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The ONE GraphicsSettings -> RendererSettings mapping (#1235). Game (fresh launch) and
// SettingsScreen (Apply) used to carry diverged copies of this conversion, and the divergence was
// user-visible: Apply forced bloom on while the next launch derived it from the quality preset, so
// a low-preset config rendered differently after Apply than after a restart. Bloom resolves here,
// deliberately, to the launch rule.

#include "RenderTypes.h"
#include "config/GraphicsSettings.h"

namespace fl {

inline RendererSettings rendererSettingsFrom(const GraphicsSettings& g) {
    RendererSettings s{};
    switch (g.vsync) {
    case VsyncMode::Off:
        s.vsync = RendererVsyncMode::Off;
        break;
    case VsyncMode::Adaptive:
        s.vsync = RendererVsyncMode::Adaptive;
        break;
    default:
        s.vsync = RendererVsyncMode::On;
        break;
    }
    // Ordinals must stay in sync with the enum definitions in both headers.
    s.aaMode = static_cast<RendererAAMode>(g.aaMode);
    s.shadowQuality = static_cast<RendererShadowQuality>(g.shadowQuality);
    s.particleDensity = static_cast<RendererParticleDensity>(g.particleDensity);
    s.aoMode = static_cast<RendererAOMode>(g.ambientOcclusion);
    s.skyQuality = static_cast<RendererSkyQuality>(g.skyQuality);
    s.autoExposure = true; // baseline HDR feature, always on
    s.bloom = (g.qualityPreset >= QualityLevel::Medium);
    switch (g.drawDistance) {
    case DrawDistance::Low:
        s.drawDistanceKm = 20.0f;
        break;
    case DrawDistance::Medium:
        s.drawDistanceKm = 50.0f;
        break;
    case DrawDistance::Ultra:
        s.drawDistanceKm = 200.0f;
        break;
    default:
        s.drawDistanceKm = 100.0f;
        break; // High
    }
    return s;
}

} // namespace fl
