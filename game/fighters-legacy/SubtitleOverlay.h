// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "audio/SubtitleQueue.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

namespace fl {

// Renders the SubtitleQueue (#704) as bottom-centre stacked HudElements — the working overlay path
// until the Phase 6 IGui renderer consumes FrameScene::subtitles directly. Owns persistent char
// buffers because HudElement::text is a non-owning string_view (the ServerNotice pattern), so it must
// outlive endFrame. Submitted as an extra overlay layer in Game.cpp each frame.
class SubtitleOverlay {
  public:
    std::span<const HudElement> build(const SubtitleQueue& queue) {
        const auto& recs = queue.records();
        const std::size_t count = std::min(recs.size(), kMaxLines);
        // Show the newest `count` lines, oldest at the top of the stack.
        const std::size_t start = recs.size() - count;
        std::size_t n = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const SubtitleRecord& r = recs[start + i];
            std::snprintf(m_buf[n].data(), m_buf[n].size(), "%s", r.text.c_str());
            HudElement& e = m_elements[n];
            e = {};
            e.type = HudElement::Type::Text;
            e.align = HudAlign::Center;
            e.x = 0.5f;
            e.y = 0.80f + 0.035f * static_cast<float>(i);
            e.scale = 1.f;
            e.r = 1.f;
            e.g = 1.f;
            e.b = 0.85f;
            // Fade the line out over its final second so it does not pop off.
            const float remaining = r.durationSec - r.elapsedSec;
            e.a = std::clamp(remaining, 0.f, 1.f);
            if (e.a <= 0.f)
                e.a = 1.f; // a still-listed record with no fade window: keep it fully visible
            e.text = m_buf[n].data();
            ++n;
        }
        return {m_elements.data(), n};
    }

  private:
    static constexpr std::size_t kMaxLines = 3; // matches SubtitleQueue::kMaxActive
    std::array<std::array<char, 200>, kMaxLines> m_buf{};
    std::array<HudElement, kMaxLines> m_elements{};
};

} // namespace fl
