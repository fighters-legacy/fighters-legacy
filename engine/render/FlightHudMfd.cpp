// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"

#include "math/Angles.h"
#include "math/Units.h"

#include "flight/LocalFrame.h" // enuBasis / headingOf

#include <cmath>
#include <glm/glm.hpp>

// FlightHud::drawMfd — the datalink radar MFD + RWR pages (#528, #642). Presentations:
//   Ppi    — a 360° plan-position indicator (the relocated #528 scope), ownship-centred, nose up.
//   BScope — azimuth (+/-60 deg about the nose) vs range, the classic search B-scope.
//   Rwr    — a dedicated threat-warning ring, emitters by bearing with a level glyph.
// The LAUNCH/LOCK threat caption is NEVER page-gated — a missile inbound must show on every page.

namespace fl {

namespace {
constexpr float kHudR = 0.0f;
constexpr float kHudG = 1.0f;
constexpr float kHudB = 0.0f;

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
    if (!radar.valid)
        return;
    const HudMfdState::Page page = c.in.mfd.page;
    const float aspect = c.aspect;
    const float rangeM = (c.in.mfd.rangeScaleM > 1.0f) ? c.in.mfd.rangeScaleM : 74080.0f;
    const float dim = 0.55f;

    constexpr float kCx = 0.14f, kCy = 0.80f, kR = 0.10f;

    const glm::mat3 enu = enuBasis(c.e.position, c.in.planetRadiusM);
    const glm::dvec3 east = glm::dvec3(enu[0]);
    const glm::dvec3 north = glm::dvec3(enu[1]);
    const float ownHdg = headingOf(c.q, c.e.position, c.in.planetRadiusM);

    // Bearing/range of a world position relative to the nose (up). Returns range fraction + rel bearing.
    auto bearingRange = [&](const glm::dvec3& worldPos, float& relBearing, float& rangeFrac) {
        const glm::dvec3 d = worldPos - glm::dvec3(c.e.position.x, c.e.position.y, c.e.position.z);
        const double eC = glm::dot(d, east);
        const double nC = glm::dot(d, north);
        const double rng = std::sqrt(eC * eC + nC * nC);
        relBearing = std::atan2(static_cast<float>(eC), static_cast<float>(nC)) - ownHdg;
        rangeFrac = static_cast<float>(rng / rangeM);
    };

    static const char* kModeName[] = {"SIL", "SRCH", "TWS", "STT"};
    const char* modeStr = (c.in.mfd.radarMode < 4) ? kModeName[c.in.mfd.radarMode] : "SRCH";

    if (page == HudMfdState::Page::Ppi) {
        // Scope box + nose tick + range/mode annunciation.
        pushLine(kCx - kR / aspect, kCy - kR, kCx + kR / aspect, kCy - kR, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(kCx - kR / aspect, kCy + kR, kCx + kR / aspect, kCy + kR, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(kCx - kR / aspect, kCy - kR, kCx - kR / aspect, kCy + kR, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(kCx + kR / aspect, kCy - kR, kCx + kR / aspect, kCy + kR, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(kCx, kCy - kR, kCx, kCy - kR + 0.02f, 1.5f, kHudR, kHudG, kHudB);
        pushText(HudAlign::Left, kCx - kR / aspect, kCy + kR + 0.02f, kHudR, kHudG * dim, kHudB, "%s %.0fnm", modeStr,
                 rangeM / kMetresPerNauticalMile<float>);
        int drawn = 0;
        for (const RadarTrack& t : radar.tracks) {
            if (drawn >= kScopeMaxTracks)
                break;
            float rel, rn;
            bearingRange(glm::dvec3(t.pos[0], t.pos[1], t.pos[2]), rel, rn);
            if (rn > 1.0f)
                continue;
            const float px = kCx + std::sin(rel) * rn * (kR / aspect);
            const float py = kCy - std::cos(rel) * rn * kR;
            float r, g, b;
            iffColor(t.ident, r, g, b);
            const float half = (t.firingQuality ? 0.008f : 0.005f);
            pushRect(px - half / aspect, py - half, px + half / aspect, py + half, r, g, b, t.ownSensor ? 1.f : 0.6f);
            ++drawn;
        }
    } else if (page == HudMfdState::Page::BScope) {
        // Azimuth (+/-60 deg) on x, range (0 at bottom, max at top) on y. Square panel in the corner.
        constexpr float kAzLimit = 1.0472f; // 60 deg
        const float x0 = kCx - kR / aspect, x1 = kCx + kR / aspect, y0 = kCy - kR, y1 = kCy + kR;
        pushLine(x0, y0, x1, y0, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(x0, y1, x1, y1, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(x0, y0, x0, y1, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(x1, y0, x1, y1, 1.0f, kHudR, kHudG * dim, kHudB);
        pushLine(kCx, y0, kCx, y1, 1.0f, kHudR, kHudG * dim, kHudB); // boresight column
        pushText(HudAlign::Left, x0, y1 + 0.02f, kHudR, kHudG * dim, kHudB, "%s %.0fnm B", modeStr,
                 rangeM / kMetresPerNauticalMile<float>);
        int drawn = 0;
        for (const RadarTrack& t : radar.tracks) {
            if (drawn >= kScopeMaxTracks)
                break;
            float rel, rn;
            bearingRange(glm::dvec3(t.pos[0], t.pos[1], t.pos[2]), rel, rn);
            rel = wrapPi(rel);
            if (rn > 1.0f || std::abs(rel) > kAzLimit)
                continue;
            const float px = kCx + (rel / kAzLimit) * (kR / aspect);
            const float py = y1 - rn * (2.0f * kR); // near range at the bottom
            float r, g, b;
            iffColor(t.ident, r, g, b);
            const float half = (t.firingQuality ? 0.008f : 0.005f);
            pushRect(px - half / aspect, py - half, px + half / aspect, py + half, r, g, b, t.ownSensor ? 1.f : 0.6f);
            ++drawn;
        }
    } else if (page == HudMfdState::Page::Rwr) {
        // Dedicated RWR azimuth ring: emitters at a fixed radius by bearing, level glyph S/L/M.
        constexpr int kSeg = 24;
        glm::vec2 prev{kCx + (kR / aspect), kCy};
        for (int i = 1; i <= kSeg; ++i) {
            const float a = static_cast<float>(i) * (6.2831853f / kSeg);
            const glm::vec2 cur{kCx + std::cos(a) * (kR / aspect), kCy + std::sin(a) * kR};
            pushLine(prev.x, prev.y, cur.x, cur.y, 1.0f, kHudR, kHudG * dim, kHudB);
            prev = cur;
        }
        pushLine(kCx, kCy - kR, kCx, kCy - kR + 0.02f, 1.5f, kHudR, kHudG, kHudB); // nose tick
        pushText(HudAlign::Center, kCx, kCy + kR + 0.03f, kHudR, kHudG * dim, kHudB, "%s", "RWR");
        for (const RwrStrobe& s : radar.strobes) {
            float rel, rn;
            bearingRange(glm::dvec3(s.emitterPos[0], s.emitterPos[1], s.emitterPos[2]), rel, rn);
            const float ex = kCx + std::sin(rel) * (kR / aspect);
            const float ey = kCy - std::cos(rel) * kR;
            const char* glyph = (s.level == kThreatLaunch) ? "M" : (s.level == kThreatLock) ? "L" : "S";
            const bool hot = (s.level >= kThreatLock);
            pushText(HudAlign::Center, ex, ey - 0.008f, 1.0f, hot ? 0.1f : 0.8f, hot ? 0.1f : 0.2f, "%s", glyph);
        }
    }

    // Page-independent threat captions.
    bool anyLock = false, anyLaunch = false;
    for (const RwrStrobe& s : radar.strobes) {
        if (s.ident == kIffFriend)
            continue; // a friendly emitter is benign
        anyLock = anyLock || (s.level >= kThreatLock);
        anyLaunch = anyLaunch || (s.level == kThreatLaunch);
    }
    // On the PPI/BScope pages the strobes also ring the scope edge (kept from #528).
    if (page == HudMfdState::Page::Ppi || page == HudMfdState::Page::BScope) {
        for (const RwrStrobe& s : radar.strobes) {
            float rel, rn;
            bearingRange(glm::dvec3(s.emitterPos[0], s.emitterPos[1], s.emitterPos[2]), rel, rn);
            const float ex = kCx + std::sin(rel) * (kR / aspect);
            const float ey = kCy - std::cos(rel) * kR;
            const bool hot = (s.level >= kThreatLock);
            pushLine(ex - 0.006f / aspect, ey, ex + 0.006f / aspect, ey, 2.0f, 1.0f, hot ? 0.1f : 0.8f,
                     hot ? 0.1f : 0.2f);
        }
    }
    if (anyLaunch)
        pushText(HudAlign::Center, kCx, kCy - kR - 0.03f, 1.0f, 0.1f, 0.1f, "%s", "RWR LAUNCH");
    else if (anyLock)
        pushText(HudAlign::Center, kCx, kCy - kR - 0.03f, 1.0f, 0.1f, 0.1f, "%s", "RWR LOCK");
}

} // namespace fl
