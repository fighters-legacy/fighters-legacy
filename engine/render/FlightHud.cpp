// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"
#include "flight/AirAngles.h"

#include "math/Units.h"

#include "flight/Atmosphere.h"    // calibratedAirspeed / machNumber (IAS vs Mach, #480)
#include "flight/LocalFrame.h"    // pitchOf / bankOf / headingOf / enuBasis / radialUp
#include "render/HudProjection.h" // worldToHud / hudAspect (#692) for the flight-path marker

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl {

// Default HUD phosphor color — bright military green.
static constexpr float kHudR = 0.0f;
static constexpr float kHudG = 1.0f;
static constexpr float kHudB = 0.0f;

// ── appenders ────────────────────────────────────────────────────────────────
// The push trio now forwards onto the shared builder (#1261). The signatures stay so the ~60 draw
// calls below are untouched; what moved is the arena, the capacity check and the field fill.
bool FlightHud::pushText(HudAlign align, float x, float y, float r, float g, float b, const char* fmt, ...) {
    char buf[kFormatBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return m_hud.text(align, x, y, r, g, b, "%s", buf);
}

bool FlightHud::pushLine(float x0, float y0, float x1, float y1, float thick, float r, float g, float b, float a) {
    return m_hud.line(x0, y0, x1, y1, thick, r, g, b, a);
}

bool FlightHud::pushRect(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    return m_hud.rect(x0, y0, x1, y1, r, g, b, a);
}

// ── dispatcher ───────────────────────────────────────────────────────────────
void FlightHud::update(const HudFrameInput& in) {
    m_hud.clear();
    const EntityRenderEntry* e = in.ownship;
    if (!e)
        return;

    const double R = in.planetRadiusM;
    const float q[4] = {e->orientation.x, e->orientation.y, e->orientation.z, e->orientation.w};
    const float altMsl = static_cast<float>(geodeticAltitude(e->position.x, e->position.y, e->position.z, R));
    const AtmosphereState atmos = computeAtmosphere(altMsl);
    const float tas =
        std::sqrt(e->velocity.x * e->velocity.x + e->velocity.y * e->velocity.y + e->velocity.z * e->velocity.z);

    // Body-frame velocity (nose = +X, up = +Y), for the load factor below.
    const glm::vec3 vBody = glm::conjugate(e->orientation) * e->velocity;
    const float aoa = aoaRad(e->orientation, e->velocity);

    // Load factor from the specific force felt: a_body (centripetal, omega x v) minus gravity in the
    // body frame, over g0. Level flight with no rotation reads exactly 1 g.
    const glm::vec3 omega = e->omega;
    const glm::vec3 aCentripetal = glm::cross(omega, vBody);
    const glm::vec3 upWorld = radialUp(e->position, R);
    const glm::vec3 gWorld = -kG0<float> * upWorld;
    const glm::vec3 gBody = glm::conjugate(e->orientation) * gWorld;
    const float loadG = glm::length(aCentripetal - gBody) / kG0<float>;

    Ctx c{in,
          *e,
          {q[0], q[1], q[2], q[3]},
          altMsl,
          calibratedAirspeed(tas, atmos) * kKnotsPerMps<float>,
          tas,
          machNumber(tas, atmos.speed_of_sound_m_s),
          pitchOf(q, e->position, R),
          bankOf(q, e->position, R),
          std::fmod(glm::degrees(headingOf(q, e->position, R)) + 360.0f, 360.0f),
          glm::degrees(aoa),
          loadG,
          in.cameraValid ? hudAspect(in.camera) : 16.0f / 9.0f};

    drawFrame(c);
    drawSpeedLadder(c);
    drawAltTape(c);
    drawHeadingTapes(c);
    drawFpmAndHorizon(c);
    drawDataBlocks(c);
    drawMfd(c);
    drawCombat(c);
}

// ── octagon combiner frame + boresight + clock + latency ─────────────────────
void FlightHud::drawFrame(Ctx& c) {
    // An octagonal HUD combiner outline centred on the screen. Eight line segments over the box
    // [0.30,0.70] x [0.22,0.78] with the corners cut.
    constexpr float l = 0.30f, r = 0.70f, t = 0.22f, b = 0.78f, cut = 0.06f;
    const float dim = 0.7f;
    const float g = kHudG * dim;
    pushLine(l + cut, t, r - cut, t, 1.0f, kHudR, g, kHudB); // top
    pushLine(r - cut, t, r, t + cut, 1.0f, kHudR, g, kHudB); // top-right cut
    pushLine(r, t + cut, r, b - cut, 1.0f, kHudR, g, kHudB); // right
    pushLine(r, b - cut, r - cut, b, 1.0f, kHudR, g, kHudB); // bottom-right cut
    pushLine(r - cut, b, l + cut, b, 1.0f, kHudR, g, kHudB); // bottom
    pushLine(l + cut, b, l, b - cut, 1.0f, kHudR, g, kHudB); // bottom-left cut
    pushLine(l, b - cut, l, t + cut, 1.0f, kHudR, g, kHudB); // left
    pushLine(l, t + cut, l + cut, t, 1.0f, kHudR, g, kHudB); // top-left cut

    // Boresight cross (gun cross) at the screen centre — the fixed aircraft-datum reference.
    constexpr float cx = 0.5f, cy = 0.5f, s = 0.012f;
    pushLine(cx - s / c.aspect, cy, cx + s / c.aspect, cy, 1.5f, kHudR, kHudG, kHudB);
    pushLine(cx, cy - s, cx, cy + s, 1.5f, kHudR, kHudG, kHudB);

    // Time-of-day clock (top-right, HH:MM) + optional latency indicator.
    const int hr = static_cast<int>(c.in.timeOfDay) % 24;
    const int mn =
        static_cast<int>((c.in.timeOfDay - static_cast<float>(static_cast<int>(c.in.timeOfDay))) * 60.0f) % 60;
    pushText(HudAlign::Right, 0.98f, 0.05f, kHudR, kHudG, kHudB, "%02d:%02d", hr, mn);
    if (c.in.showLatency && c.in.latencyMs > 0)
        pushText(HudAlign::Right, 0.98f, 0.09f, kHudR, kHudG, kHudB, "%u ms", c.in.latencyMs);
    // #576: name the CAUSE. A player whose updates are arriving late will otherwise blame their own
    // connection, restart their router, and be wrong — the server is shedding work under load and
    // there is nothing on their end to fix. Amber rather than red: degraded, not broken.
    if (c.in.serverThrottled)
        pushText(HudAlign::Right, 0.98f, 0.13f, 1.0f, 0.75f, 0.2f, "SERVER LOAD %u%%", c.in.serverLoadPct);
    if (c.in.nvgActive)
        pushText(HudAlign::Left, 0.02f, 0.05f, 0.2f, 1.0f, 0.3f, "%s", "NVG");

    // Damage warning (centre) in red.
    if (c.e.damageLevel > 0)
        pushText(HudAlign::Center, 0.5f, 0.30f, 1.0f, 0.2f, 0.2f, "%s", "*** DAMAGE ***");
}

// ── velocity ladder (left, knots) ────────────────────────────────────────────
void FlightHud::drawSpeedLadder(Ctx& c) {
    constexpr float x = 0.18f;                 // ladder column
    constexpr float halfSpan = 0.18f;          // vertical extent above/below centre
    constexpr float perKt = halfSpan / 100.0f; // 100 kt spans the half-height
    const float ias = c.iasKts;

    // Vertical scale line + a boxed current-IAS readout at centre.
    pushLine(x, 0.5f - halfSpan, x, 0.5f + halfSpan, 1.0f, kHudR, kHudG, kHudB);
    pushRect(x - 0.055f, 0.485f, x - 0.002f, 0.515f, kHudR, kHudG, kHudB, 0.15f);
    pushText(HudAlign::Right, x - 0.006f, 0.492f, kHudR, kHudG, kHudB, "%d", static_cast<int>(std::lround(ias)));
    pushText(HudAlign::Center, x, 0.5f - halfSpan - 0.03f, kHudR, kHudG, kHudB, "%s", "IAS");

    // Tick marks every 10 kt (minor) and 50 kt (major, labelled), scrolling with speed.
    const int base = static_cast<int>(std::floor(ias / 10.0f)) * 10;
    for (int kt = base - 100; kt <= base + 100; kt += 10) {
        if (kt < 0)
            continue;
        const float y = 0.5f - (static_cast<float>(kt) - ias) * perKt;
        if (y < 0.5f - halfSpan || y > 0.5f + halfSpan)
            continue;
        const bool major = (kt % 50 == 0);
        pushLine(x, y, x + (major ? 0.03f : 0.017f), y, 1.0f, kHudR, kHudG, kHudB);
        if (major)
            pushText(HudAlign::Left, x + 0.035f, y - 0.012f, kHudR, kHudG, kHudB, "%d", kt);
    }
}

// ── altitude tape (right, feet) ──────────────────────────────────────────────
void FlightHud::drawAltTape(Ctx& c) {
    constexpr float x = 0.82f;
    constexpr float halfSpan = 0.18f;
    const float altFt = c.altMsl * kFeetPerMetre<float>;
    constexpr float perFt = halfSpan / 2500.0f; // 2500 ft spans the half-height

    pushLine(x, 0.5f - halfSpan, x, 0.5f + halfSpan, 1.0f, kHudR, kHudG, kHudB);
    pushRect(x + 0.002f, 0.485f, x + 0.075f, 0.515f, kHudR, kHudG, kHudB, 0.15f);
    pushText(HudAlign::Left, x + 0.006f, 0.492f, kHudR, kHudG, kHudB, "%d", static_cast<int>(std::lround(altFt)));
    pushText(HudAlign::Center, x, 0.5f - halfSpan - 0.03f, kHudR, kHudG, kHudB, "%s", "ALT");

    // Radar altitude (AGL) below the box — the terrain-relative height a pilot flies low by. Falls
    // back to MSL when terrain is not loaded (terrainElevation == 0).
    const float aglFt = (c.altMsl - c.in.terrainElevation) * kFeetPerMetre<float>;
    pushText(HudAlign::Left, x + 0.006f, 0.53f, kHudR, kHudG, kHudB, "AGL %d", static_cast<int>(std::lround(aglFt)));

    const int base = static_cast<int>(std::floor(altFt / 500.0f)) * 500;
    for (int ft = base - 2500; ft <= base + 2500; ft += 500) {
        const float y = 0.5f - (static_cast<float>(ft) - altFt) * perFt;
        if (y < 0.5f - halfSpan || y > 0.5f + halfSpan)
            continue;
        const bool major = (ft % 1000 == 0);
        pushLine(x - (major ? 0.03f : 0.017f), y, x, y, 1.0f, kHudR, kHudG, kHudB);
        if (major)
            pushText(HudAlign::Right, x - 0.035f, y - 0.012f, kHudR, kHudG, kHudB, "%d", ft);
    }
}

// ── heading tapes (top + bottom) ─────────────────────────────────────────────
void FlightHud::drawHeadingTapes(Ctx& c) {
    const float hdg = c.hdgDeg;
    // Cardinal / intercardinal labels every 45 degrees.
    auto cardinal = [](int deg) -> const char* {
        switch (((deg % 360) + 360) % 360) {
        case 0:
            return "N";
        case 45:
            return "NE";
        case 90:
            return "E";
        case 135:
            return "SE";
        case 180:
            return "S";
        case 225:
            return "SW";
        case 270:
            return "W";
        case 315:
            return "NW";
        default:
            return nullptr;
        }
    };

    for (float tapeY : {0.09f, 0.91f}) {
        constexpr float halfSpan = 0.22f;
        constexpr float perDeg = halfSpan / 45.0f; // ±45° visible either side of the lubber
        pushLine(0.5f - halfSpan, tapeY, 0.5f + halfSpan, tapeY, 1.0f, kHudR, kHudG, kHudB);
        // Decade-aligned ticks so 0/90/180/270 (the cardinal marks) are always hit; ±40° visible.
        const int base = static_cast<int>(std::lround(hdg / 10.0f)) * 10;
        for (int d = base - 40; d <= base + 40; d += 10) {
            const float x = 0.5f + (static_cast<float>(d) - hdg) * perDeg;
            if (x < 0.5f - halfSpan || x > 0.5f + halfSpan)
                continue;
            const bool major = (d % 30 == 0);
            const float tickTop = (tapeY < 0.5f) ? tapeY : tapeY - (major ? 0.02f : 0.012f);
            const float tickBot = (tapeY < 0.5f) ? tapeY + (major ? 0.02f : 0.012f) : tapeY;
            pushLine(x, tickTop, x, tickBot, 1.0f, kHudR, kHudG, kHudB);
            if (const char* lbl = cardinal(d))
                pushText(HudAlign::Center, x, (tapeY < 0.5f) ? tapeY + 0.025f : tapeY - 0.04f, kHudR, kHudG, kHudB,
                         "%s", lbl);
        }
    }
    // Lubber line (a downward caret at the top tape) + the boxed numeric heading above it.
    pushLine(0.5f, 0.09f, 0.49f, 0.07f, 1.5f, kHudR, kHudG, kHudB);
    pushLine(0.5f, 0.09f, 0.51f, 0.07f, 1.5f, kHudR, kHudG, kHudB);
    pushText(HudAlign::Center, 0.5f, 0.03f, kHudR, kHudG, kHudB, "HDG %03d", static_cast<int>(std::lround(hdg)) % 360);
}

// ── flight-path marker + radial artificial horizon ───────────────────────────
void FlightHud::drawFpmAndHorizon(Ctx& c) {
    // Artificial horizon: a bar displaced by pitch and tilted by bank on the local-level frame (#479),
    // so it reads correctly planet-wide. Screen y grows downward, so nose-up pushes it below centre.
    {
        constexpr float kPitchGain = 0.35f;
        constexpr float kHalfWidth = 0.20f;
        const float yc = std::clamp(0.5f + c.pitchRad * kPitchGain, 0.24f, 0.76f);
        const float dx = kHalfWidth * std::cos(c.bankRad);
        const float dy = kHalfWidth * std::sin(c.bankRad) / c.aspect;
        pushLine(0.5f - dx, yc + dy, 0.5f + dx, yc - dy, 1.5f, kHudR, kHudG, kHudB);
    }

    // Flight-path marker: project a point 1000 m along the velocity vector. Below ~15 m/s the velocity
    // direction is meaningless (on the runway), so pin it to the boresight.
    glm::vec2 fpm{0.5f, 0.5f};
    bool haveFpm = false;
    if (c.in.cameraValid && c.tasMps > 15.0f) {
        const glm::dvec3 dir = glm::normalize(glm::dvec3(c.e.velocity));
        if (auto p = worldToHud(c.in.camera, c.e.position + dir * 1000.0)) {
            fpm = *p;
            haveFpm = true;
        }
    }
    if (!haveFpm)
        return; // no valid FPM this frame (stationary or off-screen) — boresight cross already drawn

    // Circle (8-segment) + three wings: left, right, and top stub.
    constexpr float rad = 0.012f;
    glm::vec2 prev{fpm.x + rad / c.aspect, fpm.y};
    for (int i = 1; i <= 8; ++i) {
        const float a = static_cast<float>(i) * (2.0f * 3.14159265f / 8.0f);
        const glm::vec2 cur{fpm.x + std::cos(a) * rad / c.aspect, fpm.y + std::sin(a) * rad};
        pushLine(prev.x, prev.y, cur.x, cur.y, 1.0f, kHudR, kHudG, kHudB);
        prev = cur;
    }
    pushLine(fpm.x - rad / c.aspect, fpm.y, fpm.x - 0.03f / c.aspect, fpm.y, 1.0f, kHudR, kHudG, kHudB); // left wing
    pushLine(fpm.x + rad / c.aspect, fpm.y, fpm.x + 0.03f / c.aspect, fpm.y, 1.0f, kHudR, kHudG, kHudB); // right wing
    pushLine(fpm.x, fpm.y - rad, fpm.x, fpm.y - 0.02f, 1.0f, kHudR, kHudG, kHudB);                       // top stub
}

// ── lower data blocks ────────────────────────────────────────────────────────
void FlightHud::drawDataBlocks(Ctx& c) {
    // Lower-left: AoA / Mach / G / fuel, stacked.
    constexpr float lx = 0.05f;
    pushText(HudAlign::Left, lx, 0.62f, kHudR, kHudG, kHudB, "a %+4.1f", c.aoaDeg);
    pushText(HudAlign::Left, lx, 0.66f, kHudR, kHudG, kHudB, "M %4.2f", c.mach);
    pushText(HudAlign::Left, lx, 0.70f, kHudR, kHudG, kHudB, "G %4.1f", c.loadG);
    pushText(HudAlign::Left, lx, 0.74f, kHudR, kHudG, kHudB, "FUEL %3d", static_cast<int>(c.e.fuelPct));

    // Lower-right: master-arm / throttle / nav placeholder. The per-station weapon block is drawn by
    // drawCombat (#641).
    constexpr float rx = 0.70f;
    pushText(HudAlign::Left, rx, 0.55f, kHudR, kHudG, kHudB, "%s", c.in.masterArm ? "ARM" : "SAFE");
    pushText(HudAlign::Left, rx, 0.59f, kHudR, kHudG, kHudB, "THR %3d%%", static_cast<int>(c.e.throttle));
    pushText(HudAlign::Left, rx, 0.63f, kHudR, kHudG, kHudB, "%s", "TCN ---");

    // Configuration annunciator (#639): gear and flaps read the actual POSITION off the ownship's
    // articulation channels, not the switch — so mid-transit reads "GEAR..." and the pilot can see
    // the gear is still travelling rather than assuming the lever position is the truth. Drawn only
    // when something is off its clean position, so a clean airframe's HUD is unchanged.
    {
        const float gear = c.e.artChannels[static_cast<std::size_t>(ArtChannel::Gear)];
        const float flap = c.e.artChannels[static_cast<std::size_t>(ArtChannel::Flaps)];
        const float hook = c.e.artChannels[static_cast<std::size_t>(ArtChannel::Hook)];
        const float brake = c.e.artChannels[static_cast<std::size_t>(ArtChannel::Speedbrake)];
        float y = 0.78f;
        if (gear > 0.001f)
            pushText(HudAlign::Left, lx, y, kHudR, kHudG, kHudB, "%s", (gear > 0.999f) ? "GEAR DN" : "GEAR ...");
        if (flap > 0.001f) {
            y += 0.04f;
            pushText(HudAlign::Left, lx, y, kHudR, kHudG, kHudB, "FLAP %3d", static_cast<int>(flap * 100.f + 0.5f));
        }
        if (brake > 0.001f) {
            y += 0.04f;
            pushText(HudAlign::Left, lx, y, kHudR, kHudG, kHudB, "%s", "SPD BRK");
        }
        if (hook > 0.001f) {
            y += 0.04f;
            pushText(HudAlign::Left, lx, y, kHudR, kHudG, kHudB, "%s", (hook > 0.999f) ? "HOOK DN" : "HOOK ...");
        }
    }

    // Seeker LOCK annunciator (#628) from the own-record weaponFlags bit 0.
    if (c.e.hasLoadout && (c.e.weaponFlags & 0x01u))
        pushText(HudAlign::Center, 0.5f, 0.40f, kHudR, kHudG, kHudB, "%s", "LOCK");

    // Autopilot annunciation (#640): a single line naming the engaged holds. Built with bounded
    // snprintf appends tracking the write offset (portable — strncat trips MSVC's C4996).
    if (c.in.apModes != 0) {
        char buf[48] = "AP";
        int len = 2;
        auto append = [&](const char* fmt, int value) {
            if (len < static_cast<int>(sizeof(buf)) - 1) {
                const int n = std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), fmt, value);
                if (n > 0)
                    len += n;
            }
        };
        if (c.in.apModes & 0x1u)
            append(" ALT%d", static_cast<int>(std::lround(c.in.apTargetAltM * kFeetPerMetre<float>)));
        if (c.in.apModes & 0x2u)
            append(" HDG%03d", static_cast<int>(std::lround(c.in.apTargetHeadingDeg)) % 360);
        if (c.in.apModes & 0x4u)
            append(" SPD%d", static_cast<int>(std::lround(c.in.apTargetSpeedMps * kKnotsPerMps<float>)));
        pushText(HudAlign::Center, 0.5f, 0.20f, kHudR, kHudG, kHudB, "%s", buf);
    }
}

// drawCombat (#641) and drawMfd (#642) live in FlightHudCombat.cpp / FlightHudMfd.cpp.

std::span<const HudElement> FlightHud::elements() const {
    return m_hud.elements();
}

} // namespace fl
