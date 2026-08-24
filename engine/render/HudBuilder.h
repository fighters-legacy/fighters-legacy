// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h" // HudElement / HudAlign

#include <array>
#include <cstdarg>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace fl {

// Assembling a HudElement list, in one place (#1261).
//
// FlightHud and GameConsole each carried a near-identical pushText/pushLine/pushRect trio -- the
// vsnprintf into a string arena, the capacity check, the field-by-field fill -- and eight screens
// carried the same thirteen-line fullscreen-background block verbatim.
//
// ⚠ THE STRING-LIFETIME RULE, stated once here instead of re-derived at five call sites:
// HudElement::text is a std::string_view, so its backing storage must outlive the frame. `text()`
// copies into the builder's own arena, which lives as long as the builder. `textView()` does NOT
// copy -- use it only for storage that already outlives the frame (a member string, a literal).

// Opacity of a timed HUD line: fully opaque for `dwellSecs`, then linear to nothing over
// `fadeSecs` (#1265).
//
// The chat overlay and the kill feed are the two timed line-stacks, and both spelled this out. Their
// dwell and fade DIFFER on purpose (10/2 versus 8/2) -- that is what makes them parameters rather
// than constants -- but the curve is one rule, and two copies of it is two chances for one stack to
// snap off while the other fades.
[[nodiscard]] inline float fadeAlpha(float ageSecs, float dwellSecs, float fadeSecs) noexcept {
    if (ageSecs <= dwellSecs)
        return 1.f;
    if (fadeSecs <= 0.f)
        return 0.f; // no fade window: the line ends the instant its dwell does
    const float t = 1.f - (ageSecs - dwellSecs) / fadeSecs;
    return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
}

// A fullscreen opaque background. Eight screens spelled this out; there is nothing to parameterise
// but the colour.
[[nodiscard]] inline HudElement hudFullscreenBg(float r = 0.f, float g = 0.f, float b = 0.f, float a = 1.f) {
    HudElement el;
    el.type = HudElement::Type::Rect;
    el.x = 0.f;
    el.y = 0.f;
    el.x2 = 1.f;
    el.y2 = 1.f;
    el.r = r;
    el.g = g;
    el.b = b;
    el.a = a;
    return el;
}

// Fixed-capacity element + string storage. Both capacities are template parameters because the two
// engine consumers sized them differently on purpose: a flight HUD draws far more symbology than a
// console draws lines.
template <std::size_t MaxElements, std::size_t MaxStrings, std::size_t FormatBytes = 128> class HudBuilder {
  public:
    void clear() noexcept {
        m_elementCount = 0;
        m_stringCount = 0;
        m_overflowed = false;
    }

    // True if any append hit a cap since the last clear(). Tests assert against this rather than
    // silently rendering a partial frame.
    [[nodiscard]] bool overflowed() const noexcept {
        return m_overflowed;
    }

    [[nodiscard]] std::span<const HudElement> elements() const noexcept {
        return {m_elements.data(), m_elementCount};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_elementCount;
    }

    // Formatted text, copied into the arena so the view stays valid for the frame.
    [[nodiscard]] bool text(HudAlign align, float x, float y, float r, float g, float b, const char* fmt, ...) {
        if (m_elementCount >= MaxElements || m_stringCount >= MaxStrings) {
            m_overflowed = true;
            return false;
        }
        char buf[FormatBytes];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        m_strings[m_stringCount] = buf;
        return pushText(align, x, y, r, g, b, m_strings[m_stringCount++]);
    }

    // Text backed by storage the CALLER guarantees outlives the frame. No copy, no arena slot.
    [[nodiscard]] bool textView(HudAlign align, float x, float y, float r, float g, float b, std::string_view stable) {
        if (m_elementCount >= MaxElements) {
            m_overflowed = true;
            return false;
        }
        return pushText(align, x, y, r, g, b, stable);
    }

    [[nodiscard]] bool line(float x0, float y0, float x1, float y1, float thick, float r, float g, float b, float a) {
        HudElement el;
        el.type = HudElement::Type::Line;
        el.x = x0;
        el.y = y0;
        el.x2 = x1;
        el.y2 = y1;
        el.strokeWidth = thick;
        el.r = r;
        el.g = g;
        el.b = b;
        el.a = a;
        return push(el);
    }

    [[nodiscard]] bool rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
        HudElement el;
        el.type = HudElement::Type::Rect;
        el.x = x0;
        el.y = y0;
        el.x2 = x1;
        el.y2 = y1;
        el.r = r;
        el.g = g;
        el.b = b;
        el.a = a;
        return push(el);
    }

    [[nodiscard]] bool fullscreenBg(float r = 0.f, float g = 0.f, float b = 0.f, float a = 1.f) {
        return push(hudFullscreenBg(r, g, b, a));
    }

    // Append a span a sub-widget already built (the flight screen's sub-menus, hudBox's four lines).
    bool append(std::span<const HudElement> els) {
        for (const HudElement& el : els) {
            if (!push(el))
                return false;
        }
        return true;
    }

  private:
    // Every append starts from a DEFAULT-CONSTRUCTED element. GameConsole used to fill a reused slot
    // in place, so stale fields survived across frames -- harmless only because console elements
    // never set align and Text ignores x2/y2. Constructing fresh makes that structurally impossible
    // rather than accidentally true.
    bool push(const HudElement& el) {
        if (m_elementCount >= MaxElements) {
            m_overflowed = true;
            return false;
        }
        m_elements[m_elementCount++] = el;
        return true;
    }

    bool pushText(HudAlign align, float x, float y, float r, float g, float b, std::string_view sv) {
        HudElement el;
        el.type = HudElement::Type::Text;
        el.x = x;
        el.y = y;
        el.align = align;
        el.r = r;
        el.g = g;
        el.b = b;
        el.text = sv;
        return push(el);
    }

    std::array<HudElement, MaxElements> m_elements{};
    std::array<std::string, MaxStrings> m_strings{};
    std::size_t m_elementCount{0};
    std::size_t m_stringCount{0};
    bool m_overflowed{false};
};

} // namespace fl
