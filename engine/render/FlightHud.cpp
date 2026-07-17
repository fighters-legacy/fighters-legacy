// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"

#include "flight/Atmosphere.h" // calibratedAirspeed / machNumber for IAS vs Mach (#480)
#include "flight/LocalFrame.h" // headingOf / pitchOf / bankOf on the local-level frame

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

namespace fl {

// Default HUD phosphor color — bright military green.
// Will be a user-configurable option in a later phase.
static constexpr float kHudR = 0.0f;
static constexpr float kHudG = 1.0f;
static constexpr float kHudB = 0.0f;

void FlightHud::update(const EntityRenderEntry* e, float timeOfDay, float terrainElevation, uint32_t latencyMs,
                       bool showLatency, double planetRadiusM) {
    m_elementCount = 0;
    m_stringCount = 0;
    if (!e)
        return;

    auto pushText = [&](HudAlign align, float x, float y, float r, float g, float b, const char* fmt, auto... args) {
        if (m_elementCount >= kMaxElements || m_stringCount >= kMaxStrings)
            return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        m_strings[m_stringCount] = buf;
        HudElement el;
        el.type = HudElement::Type::Text;
        el.x = x;
        el.align = align;
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

    // Airspeed (left side, vertically centered). Three distinct speeds diverge with altitude (#480):
    // the wire carries only world-frame velocity, so |velocity| is groundspeed and — absent wind on
    // the wire — the best available estimate of true airspeed (TAS). From TAS + the local atmosphere
    // we derive the two readouts a pilot actually flies: IAS (calibrated/indicated, dynamic-pressure)
    // and Mach (TAS ÷ local speed of sound). The old code labelled raw groundspeed "IAS", which read
    // high by the whole density lapse at altitude. 1 m/s = 1.94384 kts.
    const float altMsl =
        static_cast<float>(geodeticAltitude(e->position.x, e->position.y, e->position.z, planetRadiusM));
    const AtmosphereState atmos = computeAtmosphere(altMsl);
    const float tasMps =
        std::sqrt(e->velocity.x * e->velocity.x + e->velocity.y * e->velocity.y + e->velocity.z * e->velocity.z);
    const float iasKts = calibratedAirspeed(tasMps, atmos) * 1.94384f;
    const float mach = machNumber(tasMps, atmos.speed_of_sound_m_s);
    pushText(HudAlign::Left, 0.03f, 0.46f, kHudR, kHudG, kHudB, "IAS %5.0fkts", iasKts);

    // Altitude MSL and AGL (left side, below airspeed). Both radial: ALT is the geodetic (MSL)
    // altitude above the datum and AGL subtracts the terrain radial elevation — correct planet-wide,
    // not just where world-Y aliases altitude near the origin (#477). terrainElevation is the terrain
    // radial elevation (heightAt(dvec3)) supplied by the caller.
    pushText(HudAlign::Left, 0.03f, 0.50f, kHudR, kHudG, kHudB, "ALT %5.0fm", altMsl);
    const float agl = altMsl - terrainElevation;
    pushText(HudAlign::Left, 0.03f, 0.54f, kHudR, kHudG, kHudB, "AGL %5.0fm", agl);

    // Mach number (left column, below AGL) — reads increasingly higher than IAS-implied Mach as the
    // aircraft climbs, the key high-altitude distinction the flat "IAS" readout hid.
    pushText(HudAlign::Left, 0.03f, 0.58f, kHudR, kHudG, kHudB, "M %4.2f", mach);

    // Attitude on the LOCAL-LEVEL frame at the entity position (radial up on a spherical planet).
    // These reduce to the world-frame values near the origin but stay correct planet-wide (#479).
    const float q[4] = {e->orientation.x, e->orientation.y, e->orientation.z, e->orientation.w};
    const float pitchRad = pitchOf(q, e->position, planetRadiusM);
    const float bankRad = bankOf(q, e->position, planetRadiusM);

    // Pitch readout (left column, above airspeed)
    pushText(HudAlign::Left, 0.03f, 0.42f, kHudR, kHudG, kHudB, "PTCH %+03.0f", glm::degrees(pitchRad));

    // Heading (bottom-center) — compass bearing of the nose in the local tangent plane (0 = N, 90 = E).
    const float hdgRad = headingOf(q, e->position, planetRadiusM);
    float hdg = std::fmod(glm::degrees(hdgRad) + 360.f, 360.f);
    pushText(HudAlign::Center, 0.5f, 0.94f, kHudR, kHudG, kHudB, "HDG %3.0f", hdg);

    // Heading tape underline
    pushLine(0.35f, 0.97f, 0.65f, 0.97f, 1.f, kHudR, kHudG, kHudB);

    // Artificial horizon line: displaced vertically by pitch, tilted by bank relative to local up.
    // Screen y grows downward, so nose-up (positive pitch) pushes the horizon below centre.
    // The bank tilt is applied in normalized space with a nominal aspect so the slope reads as a
    // physical roll; the exact look is manual-flight verified.
    {
        constexpr float kPitchGain = 0.35f; // screen fraction per radian of pitch
        constexpr float kHalfWidth = 0.20f; // half-length of the horizon bar (normalized x)
        constexpr float kHudAspect = 16.f / 9.f;
        const float yc = std::clamp(0.5f + pitchRad * kPitchGain, 0.12f, 0.88f);
        const float dx = kHalfWidth * std::cos(bankRad);
        const float dy = kHalfWidth * std::sin(bankRad) / kHudAspect;
        // Right bank raises the right side of the outside horizon relative to the aircraft frame.
        pushLine(0.5f - dx, yc + dy, 0.5f + dx, yc - dy, 1.5f, kHudR, kHudG, kHudB);
    }

    // Throttle + fuel (right side, vertically centered)
    pushText(HudAlign::Left, 0.80f, 0.46f, kHudR, kHudG, kHudB, "THR %3d%%", static_cast<int>(e->throttle));
    pushText(HudAlign::Left, 0.80f, 0.50f, kHudR, kHudG, kHudB, "FUEL %3d%%", static_cast<int>(e->fuelPct));

    // Selected weapon (#440), right column below THR/FUEL: "ARM <name> x<rounds>". The numbers are
    // the server's own-record loadout block (#625); the name is a client-side label — "STA n" when
    // the client has no def to name it from. No loadout block on the wire yet = no line.
    if (e->hasLoadout && e->selectedStation != 255) {
        const std::size_t sel = e->selectedStation;
        if (sel < m_stationLabels.size() && !m_stationLabels[sel].empty())
            pushText(HudAlign::Left, 0.80f, 0.58f, kHudR, kHudG, kHudB, "ARM %s x%u", m_stationLabels[sel].c_str(),
                     static_cast<unsigned>(e->stationRounds));
        else
            pushText(HudAlign::Left, 0.80f, 0.58f, kHudR, kHudG, kHudB, "ARM STA%u x%u", static_cast<unsigned>(sel + 1),
                     static_cast<unsigned>(e->stationRounds));
    }

    // Seeker LOCK annunciator (#628) — the pre-launch growl, replicated from the own-record
    // weaponFlags bit 0: the server says the selected seeker sees the designated target right now.
    if (e->hasLoadout && (e->weaponFlags & 0x01u))
        pushText(HudAlign::Center, 0.5f, 0.42f, kHudR, kHudG, kHudB, "%s", "LOCK");

    // Damage warning in red (center screen)
    if (e->damageLevel > 0)
        pushText(HudAlign::Center, 0.5f, 0.48f, 1.f, 0.2f, 0.2f, "%s", "*** DAMAGE ***");

    // Time of day clock (top-right) — HH:MM, purely ASCII
    int hr = static_cast<int>(timeOfDay) % 24;
    int min = static_cast<int>((timeOfDay - static_cast<float>(static_cast<int>(timeOfDay))) * 60.f) % 60;
    pushText(HudAlign::Right, 0.98f, 0.38f, kHudR, kHudG, kHudB, "%02d:%02d", hr, min);

    // Per-peer latency indicator (right column, below fuel) — e.g. "42 ms"
    if (showLatency && latencyMs > 0)
        pushText(HudAlign::Left, 0.80f, 0.54f, kHudR, kHudG, kHudB, "%u ms", latencyMs);
}

std::span<const HudElement> FlightHud::elements() const {
    return {m_elements.data(), m_elementCount};
}

} // namespace fl
