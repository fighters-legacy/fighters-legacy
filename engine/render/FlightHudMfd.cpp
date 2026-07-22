// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"

#include "flight/LocalFrame.h" // enuBasis / headingOf

#include <cmath>
#include <glm/glm.hpp>

// FlightHud::drawMfd — the datalink radar MFD + RWR (#528, #642). Split out of FlightHud.cpp so the
// MFD-page presentations (#642) grow here without bloating the core instrument file. This file holds
// the relocated #528 PPI (the Ppi page); the B-scope and dedicated RWR pages are added by #642.

namespace fl {

namespace {
constexpr float kHudR = 0.0f;
constexpr float kHudG = 1.0f;
constexpr float kHudB = 0.0f;

// IFF colour for a track/strobe ordinal (kIff*): green friend, red foe, amber unknown.
void iffColor(uint8_t ident, float& r, float& g, float& b) {
    r = 1.0f;
    g = 1.0f;
    b = 0.3f; // Unknown = amber
    if (ident == kIffFriend) {
        r = 0.2f;
        g = 1.0f;
        b = 0.4f;
    } else if (ident == kIffFoe) {
        r = 1.0f;
        g = 0.2f;
        b = 0.2f;
    }
}
} // namespace

void FlightHud::drawMfd(Ctx& c) {
    const RadarView& radar = c.in.radar;
    if (!radar.valid || c.in.mfd.page == HudMfdState::Page::Off)
        return;

    // A 360° PPI in the lower-left: ownship at centre, nose up, the fused TEAM picture plotted by
    // bearing and range, coloured by IFF. RWR strobes ring the scope edge at the emitter bearing.
    constexpr float kCx = 0.14f, kCy = 0.80f, kR = 0.10f;
    const float aspect = c.aspect;
    const float rangeM = (c.in.mfd.rangeScaleM > 1.0f) ? c.in.mfd.rangeScaleM : 74080.0f;
    const float dim = 0.55f;

    // Scope frame + nose tick + range caption + radar-mode annunciation.
    pushLine(kCx - kR / aspect, kCy - kR, kCx + kR / aspect, kCy - kR, 1.0f, kHudR, kHudG * dim, kHudB);
    pushLine(kCx - kR / aspect, kCy + kR, kCx + kR / aspect, kCy + kR, 1.0f, kHudR, kHudG * dim, kHudB);
    pushLine(kCx - kR / aspect, kCy - kR, kCx - kR / aspect, kCy + kR, 1.0f, kHudR, kHudG * dim, kHudB);
    pushLine(kCx + kR / aspect, kCy - kR, kCx + kR / aspect, kCy + kR, 1.0f, kHudR, kHudG * dim, kHudB);
    pushLine(kCx, kCy - kR, kCx, kCy - kR + 0.02f, 1.5f, kHudR, kHudG, kHudB); // nose tick (up = ahead)

    static const char* kModeName[] = {"SIL", "SRCH", "TWS", "STT"};
    const char* modeStr = (c.in.mfd.radarMode < 4) ? kModeName[c.in.mfd.radarMode] : "SRCH";
    pushText(HudAlign::Left, kCx - kR / aspect, kCy + kR + 0.02f, kHudR, kHudG * dim, kHudB, "%s %.0fnm", modeStr,
             rangeM / 1852.0f);

    const glm::mat3 enu = enuBasis(c.e.position, c.in.planetRadiusM);
    const glm::dvec3 east = glm::dvec3(enu[0]);
    const glm::dvec3 north = glm::dvec3(enu[1]);
    const float ownHdg = headingOf(c.q, c.e.position, c.in.planetRadiusM);

    auto plot = [&](const glm::dvec3& worldPos, float& outX, float& outY) -> bool {
        const glm::dvec3 d = worldPos - glm::dvec3(c.e.position.x, c.e.position.y, c.e.position.z);
        const double eC = glm::dot(d, east);
        const double nC = glm::dot(d, north);
        const double rng = std::sqrt(eC * eC + nC * nC);
        if (rng > rangeM)
            return false;
        const float bearing = std::atan2(static_cast<float>(eC), static_cast<float>(nC)); // 0 = N
        const float rel = bearing - ownHdg;                                               // relative to the nose (up)
        const float rN = static_cast<float>(rng / rangeM);
        outX = kCx + std::sin(rel) * rN * (kR / aspect);
        outY = kCy - std::cos(rel) * rN * kR; // screen y grows downward; forward = up
        return true;
    };

    int drawn = 0;
    for (const RadarTrack& t : radar.tracks) {
        if (drawn >= kScopeMaxTracks)
            break;
        float px, py;
        if (!plot(glm::dvec3(t.pos[0], t.pos[1], t.pos[2]), px, py))
            continue;
        float r, g, b;
        iffColor(t.ident, r, g, b);
        const float alpha = t.ownSensor ? 1.0f : 0.6f;
        const float half = (t.firingQuality ? 0.008f : 0.005f);
        pushRect(px - half / aspect, py - half, px + half / aspect, py + half, r, g, b, alpha);
        ++drawn;
    }

    // RWR strobes ring the scope edge at the emitter bearing; scans amber, lock/launch red. A launch
    // (a guided missile inbound) wins the caption over a bare lock. Captions are NEVER page-gated.
    bool anyLock = false, anyLaunch = false;
    for (const RwrStrobe& s : radar.strobes) {
        const glm::dvec3 d = glm::dvec3(s.emitterPos[0], s.emitterPos[1], s.emitterPos[2]) -
                             glm::dvec3(c.e.position.x, c.e.position.y, c.e.position.z);
        const double eC = glm::dot(d, east);
        const double nC = glm::dot(d, north);
        const float bearing = std::atan2(static_cast<float>(eC), static_cast<float>(nC));
        const float rel = bearing - ownHdg;
        const float ex = kCx + std::sin(rel) * (kR / aspect);
        const float ey = kCy - std::cos(rel) * kR;
        const bool lock = (s.level >= kThreatLock);
        anyLock = anyLock || lock;
        anyLaunch = anyLaunch || (s.level == kThreatLaunch);
        pushLine(ex - 0.006f / aspect, ey, ex + 0.006f / aspect, ey, 2.0f, 1.0f, lock ? 0.1f : 0.8f,
                 lock ? 0.1f : 0.2f);
    }
    if (anyLaunch)
        pushText(HudAlign::Center, kCx, kCy + kR + 0.05f, 1.0f, 0.1f, 0.1f, "%s", "RWR LAUNCH");
    else if (anyLock)
        pushText(HudAlign::Center, kCx, kCy + kR + 0.05f, 1.0f, 0.1f, 0.1f, "%s", "RWR LOCK");
}

} // namespace fl
