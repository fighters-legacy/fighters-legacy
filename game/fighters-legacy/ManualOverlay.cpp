// SPDX-License-Identifier: GPL-3.0-or-later
#include "ManualOverlay.h"

#include <algorithm>

namespace fl {
namespace {

constexpr int kVisibleLines = 26;
constexpr float kLineHeight = 0.031f;
constexpr float kTopY = 0.075f;
constexpr float kLabelX = 0.10f;
constexpr float kValueX = 0.52f;
constexpr float kTextScale = 1.0f;

} // namespace

void ManualOverlay::setManual(AircraftManual manual) {
    m_manual = std::move(manual);
    m_scroll = 0;
}

void ManualOverlay::toggle() noexcept {
    m_open = !m_open;
    if (!m_open)
        m_scroll = 0;
}

void ManualOverlay::close() noexcept {
    m_open = false;
    m_scroll = 0;
}

void ManualOverlay::scroll(int lines) noexcept {
    m_scroll = std::max(0, m_scroll + lines);
}

void ManualOverlay::update() {
    m_elements.clear();
    if (!m_open || !hasManual())
        return;

    // Flatten the manual into a line list ONCE per frame. The strings live in m_manual, which is
    // stable for the life of the overlay -- HudElement::text is a string_view, so the backing storage
    // must outlive the frame, and pointing it at a temporary is how you get a garbled HUD.
    struct Line {
        std::string_view left;
        std::string_view right;
        bool heading{false};
    };
    std::vector<Line> lines;
    lines.push_back({m_manual.title, {}, true});

    for (const auto& sec : m_manual.sections) {
        lines.push_back({{}, {}, false}); // blank
        lines.push_back({sec.title, {}, true});
        for (const auto& row : sec.rows)
            lines.push_back({row.label, row.value, false});
    }
    if (!m_manual.prose.empty()) {
        lines.push_back({{}, {}, false});
        for (const auto& p : m_manual.prose)
            lines.push_back({p, {}, false});
    }

    const int maxScroll = std::max(0, static_cast<int>(lines.size()) - kVisibleLines);
    m_scroll = std::min(m_scroll, maxScroll);

    // Dim backdrop, so the manual is readable against terrain and sky.
    HudElement bg;
    bg.type = HudElement::Type::Rect;
    bg.x = 0.06f;
    bg.y = 0.04f;
    bg.x2 = 0.94f;
    bg.y2 = 0.94f;
    bg.r = 0.02f;
    bg.g = 0.04f;
    bg.b = 0.02f;
    bg.a = 0.86f;
    m_elements.push_back(bg);

    float y = kTopY;
    const int first = m_scroll;
    const int last = std::min(static_cast<int>(lines.size()), first + kVisibleLines);

    for (int i = first; i < last; ++i) {
        const Line& ln = lines[i];

        if (!ln.left.empty()) {
            HudElement e;
            e.type = HudElement::Type::Text;
            e.x = kLabelX;
            e.y = y;
            e.scale = kTextScale;
            e.text = ln.left;
            if (ln.heading) {
                e.r = 1.0f;
                e.g = 0.85f;
                e.b = 0.35f; // headings in amber
            } else {
                e.r = 0.72f;
                e.g = 0.94f;
                e.b = 0.72f; // military green, matching the rest of the HUD
            }
            m_elements.push_back(e);
        }

        if (!ln.right.empty()) {
            HudElement v;
            v.type = HudElement::Type::Text;
            v.x = kValueX;
            v.y = y;
            v.scale = kTextScale;
            v.text = ln.right;
            v.r = 1.0f;
            v.g = 1.0f;
            v.b = 1.0f;
            m_elements.push_back(v);
        }

        y += kLineHeight;
    }

    // Footer: the manual is scrollable and most aircraft do not fit on one page.
    HudElement footer;
    footer.type = HudElement::Type::Text;
    footer.x = 0.5f;
    footer.y = 0.955f;
    footer.align = HudAlign::Center;
    footer.scale = 1.0f;
    footer.r = 0.6f;
    footer.g = 0.6f;
    footer.b = 0.6f;
    footer.text = (maxScroll > 0) ? "PgUp/PgDn to scroll   —   M or Esc to close" : "M or Esc to close";
    m_elements.push_back(footer);
}

} // namespace fl
