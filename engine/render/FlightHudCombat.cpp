// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"

#include "flight/BallisticLead.h" // computeBallisticLead / computeCcip (#641)
#include "flight/Geodetic.h"      // geodeticAltitude
#include "flight/LocalFrame.h"    // radialUp
#include "render/HudProjection.h" // worldToHud

#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

// FlightHud::drawCombat — combat symbology (#641): the gun pipper with ballistic lead, the CCIP bomb
// release cue, and the multi-station weapon-status block. The target designator BOX is drawn by
// FlightScreen (it owns the type-name registry + IFF); this file draws the ownship-relative gunnery
// cues, gated by master-arm.

namespace fl {

namespace {
constexpr float kHudR = 0.0f;
constexpr float kHudG = 1.0f;
constexpr float kHudB = 0.0f;
} // namespace

void FlightHud::drawCombat(Ctx& c) {
    // Selected-station facts for the pipper / CCIP.
    const bool haveSel = c.e.hasLoadout && c.e.selectedStation != 255 &&
                         static_cast<std::size_t>(c.e.selectedStation) < m_stations.size();
    const HudStationInfo* sel = haveSel ? &m_stations[c.e.selectedStation] : nullptr;

    const double R = c.in.planetRadiusM;
    const glm::vec3 up = radialUp(c.e.position, R);
    const glm::vec3 gravity = -9.80665f * up;

    // ── gun pipper (ballistic lead) ──────────────────────────────────────────
    if (c.in.cameraValid && c.in.masterArm && c.in.designatedTarget && sel && sel->kind == 1 /*gun*/ &&
        sel->muzzleVelMps > 1.0f) {
        const BallisticLeadResult lead =
            computeBallisticLead(c.e.position, c.e.velocity, c.in.designatedTarget->position,
                                 c.in.designatedTarget->velocity, sel->muzzleVelMps, gravity);
        if (lead.valid) {
            if (auto p = worldToHud(c.in.camera, lead.aimPoint);
                p && p->x > 0.02f && p->x < 0.98f && p->y > 0.02f && p->y < 0.98f) {
                constexpr float rad = 0.010f;
                glm::vec2 prev{p->x + rad / c.aspect, p->y};
                for (int i = 1; i <= 8; ++i) {
                    const float a = static_cast<float>(i) * (2.0f * 3.14159265f / 8.0f);
                    const glm::vec2 cur{p->x + std::cos(a) * rad / c.aspect, p->y + std::sin(a) * rad};
                    pushLine(prev.x, prev.y, cur.x, cur.y, 1.5f, kHudR, kHudG, kHudB);
                    prev = cur;
                }
                pushLine(p->x, p->y, p->x, p->y, 2.0f, kHudR, kHudG, kHudB); // centre dot
            }
        }
    }

    // ── CCIP bomb release cue ────────────────────────────────────────────────
    if (c.in.cameraValid && c.in.masterArm && sel && sel->kind == 3 /*bomb*/ && c.in.terrainHeightAt) {
        const auto heightAboveGround = [&](const glm::dvec3& pos) -> double {
            return geodeticAltitude(pos.x, pos.y, pos.z, R) - c.in.terrainHeightAt(pos);
        };
        const CcipResult ccip = computeCcip(c.e.position, c.e.velocity, glm::vec3{0.0f}, sel->dragDecayPerS, gravity,
                                            heightAboveGround, /*maxFallTimeS=*/30.0f);
        if (ccip.valid) {
            if (auto p = worldToHud(c.in.camera, ccip.impact);
                p && p->x > 0.02f && p->x < 0.98f && p->y > 0.02f && p->y < 0.98f) {
                constexpr float s = 0.012f;
                pushLine(p->x - s / c.aspect, p->y, p->x + s / c.aspect, p->y, 1.5f, kHudR, kHudG, kHudB);
                pushLine(p->x, p->y - s, p->x, p->y + s, 1.5f, kHudR, kHudG, kHudB);
                pushLine(0.5f, 0.5f, p->x, p->y, 1.0f, kHudR, kHudG, kHudB); // fall line from boresight
                pushText(HudAlign::Center, p->x, p->y + 0.03f, kHudR, kHudG, kHudB, "%.1fs", ccip.timeOfFallS);
            }
        }
    }

    // ── weapon-status block (all stations) ───────────────────────────────────
    // Lower-right, one row per station. The selected row is bracketed; the selected station's live
    // round count is the server-authoritative EntityRenderEntry::stationRounds, others show "x--"
    // until per-station ammo rides the wire (a #641 follow-up; the sentinel keeps the block honest).
    if (!m_stations.empty()) {
        constexpr float x = 0.70f;
        float y = 0.80f;
        for (std::size_t i = 0; i < m_stations.size() && i < 8; ++i) {
            const bool selected = (c.e.hasLoadout && c.e.selectedStation == i);
            const char* label = m_stations[i].label.empty() ? "----" : m_stations[i].label.c_str();
            char rounds[8] = "--";
            if (selected)
                std::snprintf(rounds, sizeof(rounds), "%u", static_cast<unsigned>(c.e.stationRounds));
            const float g = selected ? kHudG : kHudG * 0.6f;
            if (selected)
                pushText(HudAlign::Left, x, y, kHudR, g, kHudB, "[%zu] %s x%s", i + 1, label, rounds);
            else
                pushText(HudAlign::Left, x, y, kHudR, g, kHudB, " %zu  %s x%s", i + 1, label, rounds);
            y += 0.025f;
        }
    }
}

} // namespace fl
