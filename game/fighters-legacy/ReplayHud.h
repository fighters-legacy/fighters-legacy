// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "ReplayPlayer.h"

#include <array>
#include <cstdio>
#include <span>
#include <string>

// ReplayHud (#41) — the transport bar a replay is watched through, plus photo mode's readout.
//
// Header-only and pure: it turns ReplayPlayer state into HudElements and nothing else. Input handling
// stays in FlightScreen (which owns the key edges), and the camera stays in CameraInput, so this adds
// no new ownership to a screen that already has plenty.
//
// The scrub bar draws its KEYFRAMES as ticks. That is not decoration: a seek lands on the keyframe at
// or before the target, so the marks show a player where a scrub will actually land.

namespace fl {

// Photo-mode state (#41). Lives here because the HUD and FlightScreen both need it and the renderer
// reads only its results (FOV into the view matrix, EV into RendererSettings).
struct PhotoModeState {
    bool active{false};
    float fovDeg{60.f};  // clamped [20, 120] -- the MissionShot precedent
    float rollDeg{0.f};  // camera roll about the view axis, [-180, 180]
    float evOffset{0.f}; // exposure compensation in stops, [-4, +4]
};

class ReplayHud {
  public:
    static constexpr float kMinFovDeg = 20.f;
    static constexpr float kMaxFovDeg = 120.f;
    static constexpr float kMaxEv = 4.f;

    // Build the overlay for the current playback state. `photo` may be inactive, in which case only
    // the transport bar is drawn.
    std::span<const HudElement> build(const ReplayPlayer& player, const PhotoModeState& photo) {
        m_count = 0;

        constexpr float kBarY = 0.93f;
        constexpr float kBarX0 = 0.12f;
        constexpr float kBarX1 = 0.88f;
        constexpr float kBarH = 0.006f;

        // Track
        pushRect(kBarX0, kBarY, kBarX1, kBarY + kBarH, 0.25f, 0.25f, 0.25f, 0.8f);

        // Played portion
        const auto progress = static_cast<float>(player.progress());
        const float px = kBarX0 + (kBarX1 - kBarX0) * progress;
        pushRect(kBarX0, kBarY, px, kBarY + kBarH, 0.2f, 0.9f, 0.2f, 0.9f);

        // Keyframe ticks: where a scrub can land.
        const uint64_t first = player.firstTick();
        const uint64_t last = player.lastTick();
        if (last > first) {
            const auto span = static_cast<double>(last - first);
            for (const auto& e : player.keyframeTicks()) {
                if (e < first || e > last)
                    continue;
                const auto f = static_cast<float>(static_cast<double>(e - first) / span);
                const float x = kBarX0 + (kBarX1 - kBarX0) * f;
                pushRect(x, kBarY - 0.004f, x + 0.0012f, kBarY, 0.55f, 0.55f, 0.55f, 0.7f);
                if (m_count >= kMaxElements - 8)
                    break; // leave room for the text below; a long replay's ticks merge anyway
            }
        }

        // Playhead
        pushRect(px - 0.0015f, kBarY - 0.008f, px + 0.0015f, kBarY + kBarH + 0.008f, 1.f, 1.f, 1.f, 1.f);

        // "12:34 / 45:00   x1.0   PAUSED"
        std::snprintf(m_timeBuf, sizeof(m_timeBuf), "%s / %s   %s%s", clock(player.elapsedSeconds()).c_str(),
                      clock(player.durationSeconds()).c_str(), rateText(player.rate()),
                      player.paused() ? "   PAUSED" : "");
        m_timeStr = m_timeBuf;
        pushText(m_timeStr, 0.5f, kBarY - 0.035f, 1.f, 0.85f, 0.85f, 0.85f);

        if (photo.active) {
            std::snprintf(m_photoBuf, sizeof(m_photoBuf), "PHOTO   FOV %.0f\xc2\xb0   ROLL %+.0f\xc2\xb0   EV %+.1f",
                          static_cast<double>(photo.fovDeg), static_cast<double>(photo.rollDeg),
                          static_cast<double>(photo.evOffset));
            m_photoStr = m_photoBuf;
            pushText(m_photoStr, 0.5f, 0.06f, 1.f, 1.f, 0.9f, 0.4f);
        }

        return {m_elements.data(), static_cast<std::size_t>(m_count)};
    }

  private:
    static std::string clock(double seconds) {
        if (seconds < 0.0)
            seconds = 0.0;
        const auto t = static_cast<int>(seconds);
        char b[24];
        std::snprintf(b, sizeof(b), "%d:%02d", t / 60, t % 60);
        return b;
    }

    static const char* rateText(TimeRate r) {
        switch (r) {
        case TimeRate::Paused:
            return "x1.0";
        case TimeRate::Eighth:
            return "x0.125";
        case TimeRate::Quarter:
            return "x0.25";
        case TimeRate::Half:
            return "x0.5";
        case TimeRate::Normal:
            return "x1.0";
        case TimeRate::Double:
            return "x2.0";
        case TimeRate::Quad:
            return "x4.0";
        case TimeRate::Octa:
            return "x8.0";
        }
        return "x1.0";
    }

    void pushRect(float x, float y, float x2, float y2, float r, float g, float b, float a) {
        if (m_count >= kMaxElements)
            return;
        auto& el = m_elements[static_cast<std::size_t>(m_count++)];
        el = HudElement{};
        el.type = HudElement::Type::Rect;
        el.x = x;
        el.y = y;
        el.x2 = x2;
        el.y2 = y2;
        el.r = r;
        el.g = g;
        el.b = b;
        el.a = a;
    }

    void pushText(const std::string& text, float x, float y, float scale, float r, float g, float b) {
        if (m_count >= kMaxElements)
            return;
        auto& el = m_elements[static_cast<std::size_t>(m_count++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = text;
        el.x = x;
        el.align = HudAlign::Center;
        el.y = y;
        el.scale = scale;
        el.r = r;
        el.g = g;
        el.b = b;
        el.a = 1.f;
    }

    static constexpr int kMaxElements = 64;
    std::array<HudElement, kMaxElements> m_elements{};
    int m_count{0};
    char m_timeBuf[96]{};
    char m_photoBuf[96]{};
    std::string m_timeStr;
    std::string m_photoStr;
};

} // namespace fl
