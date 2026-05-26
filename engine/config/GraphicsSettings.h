// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

enum class VsyncMode : uint8_t { Off, On, Adaptive };
enum class FrameRateCap : uint8_t { Off, Cap30, Cap60, Cap120, Cap144, Cap240 };
enum class QualityLevel : uint8_t { Low, Medium, High, Ultra };
enum class DrawDistance : uint8_t { Low, Medium, High, Ultra };
enum class UiScale : uint8_t { Scale75, Scale100, Scale125, Scale150 };

struct GraphicsSettings {
    int resolutionWidth = 0;  // 0 = native
    int resolutionHeight = 0; // 0 = native
    VsyncMode vsync = VsyncMode::On;
    FrameRateCap frameRateCap = FrameRateCap::Off;
    QualityLevel qualityPreset = QualityLevel::High;
    DrawDistance drawDistance = DrawDistance::High;
    bool antiAliasing = true; // on = engine picks best method
    UiScale uiScale = UiScale::Scale100;
    int cockpitFov = 90; // degrees, clamped [60, 120]
};
