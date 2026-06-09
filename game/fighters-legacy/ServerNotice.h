// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

// Server shutdown / status notice banner.
// Shown in any camera mode. Set by ClientNetEventHandler on MsgServerNotice.
class ServerNotice {
  public:
    void setNotice(std::string_view text, uint16_t /*secondsRemaining*/) {
        std::snprintf(m_buf, sizeof(m_buf), "%.*s", static_cast<int>(text.size()), text.data());
        m_active = true;
    }

    [[nodiscard]] std::span<const HudElement> buildElements() {
        if (!m_active)
            return {};
        m_elem = {};
        m_elem.type = HudElement::Type::Text;
        m_elem.x = 0.5f;
        m_elem.y = 0.02f;
        m_elem.r = 1.f;
        m_elem.g = 0.9f;
        m_elem.b = 0.1f;
        m_elem.a = 1.f;
        m_elem.scale = 1.f;
        m_elem.text = m_buf;
        return {&m_elem, 1};
    }

  private:
    char m_buf[72]{};
    bool m_active{false};
    HudElement m_elem{};
};
