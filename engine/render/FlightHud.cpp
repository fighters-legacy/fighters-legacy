// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"

#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

namespace fl {

// Default HUD phosphor color — bright military green.
// Will be a user-configurable option in a later phase.
static constexpr float kHudR = 0.0f;
static constexpr float kHudG = 1.0f;
static constexpr float kHudB = 0.0f;

void FlightHud::update(const EntityRenderEntry* e) {
    m_elementCount = 0;
    m_stringCount = 0;
    if (!e)
        return;

    auto pushText = [&](float x, float y, float r, float g, float b, const char* fmt, auto... args) {
        if (m_elementCount >= kMaxElements || m_stringCount >= kMaxStrings)
            return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        m_strings[m_stringCount] = buf;
        HudElement el;
        el.type = HudElement::Type::Text;
        el.x = x;
        el.y = y;
        el.r = r;
        el.g = g;
        el.b = b;
        el.a = 1.f;
        el.text = m_strings[m_stringCount];
        m_elements[m_elementCount++] = el;
        ++m_stringCount;
    };

    auto pushLine = [&](float x0, float y0, float x1, float y1, float thick, float r, float g, float b) {
        if (m_elementCount >= kMaxElements)
            return;
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
        el.a = 1.f;
        m_elements[m_elementCount++] = el;
    };

    // Airspeed (left side, vertically centered) — 1 m/s = 1.94384 kts
    float speedKts =
        std::sqrt(e->velocity.x * e->velocity.x + e->velocity.y * e->velocity.y + e->velocity.z * e->velocity.z) *
        1.94384f;
    pushText(0.03f, 0.46f, kHudR, kHudG, kHudB, "IAS %5.0fkts", speedKts);

    // Altitude (left side, below airspeed)
    pushText(0.03f, 0.50f, kHudR, kHudG, kHudB, "ALT %5.0fm", static_cast<float>(e->position.y));

    // Heading (bottom-center)
    // Yaw from GLM quaternion (Y-up RH): atan2(2*(w*y + x*z), 1 - 2*(y² + z²))
    float yawRad = std::atan2(2.f * (e->orientation.w * e->orientation.y + e->orientation.x * e->orientation.z),
                              1.f - 2.f * (e->orientation.y * e->orientation.y + e->orientation.z * e->orientation.z));
    float hdg = std::fmod(glm::degrees(yawRad) + 360.f, 360.f);
    pushText(0.44f, 0.94f, kHudR, kHudG, kHudB, "HDG %3.0f", hdg);

    // Heading tape underline
    pushLine(0.35f, 0.97f, 0.65f, 0.97f, 1.f, kHudR, kHudG, kHudB);

    // Throttle + fuel (right side, vertically centered)
    pushText(0.80f, 0.46f, kHudR, kHudG, kHudB, "THR %3d%%", static_cast<int>(e->throttle));
    pushText(0.80f, 0.50f, kHudR, kHudG, kHudB, "FUEL %3d%%", static_cast<int>(e->fuelPct));

    // Damage warning in red (center screen)
    if (e->damageLevel > 0)
        pushText(0.40f, 0.48f, 1.f, 0.2f, 0.2f, "%s", "*** DAMAGE ***");
}

std::span<const HudElement> FlightHud::elements() const {
    return {m_elements.data(), m_elementCount};
}

} // namespace fl
