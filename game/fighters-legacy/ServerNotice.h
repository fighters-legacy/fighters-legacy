// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string_view>

// Server shutdown / status notice banner.
// Shown in any camera mode. Set by ClientNetEventHandler on MsgServerNotice.
// visibleSeconds > 0: fades the banner out over the final kFadeSecs then hides it.
// visibleSeconds == 0: banner persists until replaced (used by shutdown countdown).
class ServerNotice {
  public:
    void setNotice(std::string_view text, uint16_t /*secondsRemaining*/, uint32_t visibleSeconds = 0) {
        std::snprintf(m_buf, sizeof(m_buf), "%.*s", static_cast<int>(text.size()), text.data());
        m_active = true;
        if (visibleSeconds > 0) {
            m_expiry = m_now() + std::chrono::seconds(visibleSeconds);
            m_fadeStart = m_expiry - std::chrono::seconds(static_cast<long long>(kFadeSecs));
            m_hasExpiry = true;
        } else {
            m_hasExpiry = false;
        }
    }

    void setClockOverride(std::function<std::chrono::steady_clock::time_point()> fn) {
        m_now = std::move(fn);
    }

    [[nodiscard]] std::span<const HudElement> buildElements() {
        if (!m_active)
            return {};
        float alpha = 1.f;
        if (m_hasExpiry) {
            const auto now = m_now();
            if (now >= m_expiry) {
                m_active = false;
                return {};
            }
            if (now >= m_fadeStart) {
                const float elapsed = std::chrono::duration<float>(now - m_fadeStart).count();
                alpha = std::clamp(1.0f - elapsed / kFadeSecs, 0.0f, 1.0f);
            }
        }
        m_elem = {};
        m_elem.type = HudElement::Type::Text;
        m_elem.x = 0.5f;
        m_elem.y = 0.02f;
        m_elem.r = 1.f;
        m_elem.g = 0.9f;
        m_elem.b = 0.1f;
        m_elem.a = alpha;
        m_elem.scale = 1.f;
        m_elem.text = m_buf;
        return {&m_elem, 1};
    }

  private:
    static constexpr float kFadeSecs = 2.f;

    std::function<std::chrono::steady_clock::time_point()> m_now{std::chrono::steady_clock::now};
    std::chrono::steady_clock::time_point m_expiry{};
    std::chrono::steady_clock::time_point m_fadeStart{};
    bool m_hasExpiry{false};
    char m_buf[72]{};
    bool m_active{false};
    HudElement m_elem{};
};
