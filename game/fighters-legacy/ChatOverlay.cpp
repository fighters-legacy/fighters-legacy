// SPDX-License-Identifier: GPL-3.0-or-later
#include "ChatOverlay.h"

#include "IGui.h"
#include "render/HudBuilder.h" // fadeAlpha — the one timed-line fade curve (#1265)

#include <algorithm>
#include <cstdio>

namespace fl {

void ChatOverlay::pushLine(std::string_view sender, std::string_view text, ChatChannel channel, bool system) {
    if (!system && !sender.empty() && isMuted(sender))
        return; // locally muted callsign

    Line line;
    line.spawn = m_clock->now();
    if (system) {
        std::snprintf(line.buf.data(), line.buf.size(), "%.*s", static_cast<int>(text.size()), text.data());
        line.r = 1.0f;
        line.g = 0.85f;
        line.b = 0.3f; // amber for system lines
    } else {
        const char* tag = channel == ChatChannel::Team ? "[Team] " : "";
        std::snprintf(line.buf.data(), line.buf.size(), "%s%.*s: %.*s", tag, static_cast<int>(sender.size()),
                      sender.data(), static_cast<int>(text.size()), text.data());
        if (channel == ChatChannel::Team) {
            line.r = 0.45f;
            line.g = 1.0f;
            line.b = 0.6f; // green team chat
        } else {
            line.r = 0.9f;
            line.g = 0.9f;
            line.b = 0.9f; // white all chat
        }
    }
    m_lines.push_back(std::move(line));
    if (m_lines.size() > kMaxLines)
        m_lines.erase(m_lines.begin());
}

std::span<const HudElement> ChatOverlay::buildElements() {
    const auto now = m_clock->now();
    // Drop expired lines (oldest-first).
    while (!m_lines.empty()) {
        const float age = std::chrono::duration<float>(now - m_lines.front().spawn).count();
        if (age >= kLifeSecs)
            m_lines.erase(m_lines.begin());
        else
            break;
    }

    const std::size_t n = std::min(m_lines.size(), kMaxLines);
    constexpr float kBottom = 0.72f; // newest line sits here; older lines stack upward
    constexpr float kStep = 0.03f;
    for (std::size_t i = 0; i < n; ++i) {
        const Line& l = m_lines[i];
        const float age = std::chrono::duration<float>(now - l.spawn).count();
        const float alpha = fadeAlpha(age, kDwellSecs, kFadeSecs);
        HudElement& e = m_elems[i];
        e = {};
        e.type = HudElement::Type::Text;
        e.align = HudAlign::Left;
        e.x = 0.02f;
        e.y = kBottom - kStep * static_cast<float>(n - 1 - i);
        e.scale = 1.f;
        e.r = l.r;
        e.g = l.g;
        e.b = l.b;
        e.a = alpha;
        e.text = l.buf.data();
    }
    return {m_elems.data(), n};
}

void ChatOverlay::clear() noexcept {
    m_lines.clear();
}

bool ChatOverlay::renderInput(IGui* guiPtr) {
    if (!m_inputOpen || !guiPtr)
        return false;
    IGui& gui = *guiPtr;
    bool send = false;
    if (gui.beginWindow("Chat", 0.02f, 0.74f, 0.55f, 0.10f)) {
        gui.label(m_channel == ChatChannel::Team ? "Team message:" : "All message:");
        gui.inputText("##chat", m_buf, sizeof(m_buf));
        gui.sameLine();
        if (gui.button("Send"))
            send = true;
    }
    gui.endWindow();
    return send;
}

} // namespace fl
