// SPDX-License-Identifier: GPL-3.0-or-later
#include "GmMapOverlay.h"

#include "ClientNetEventHandler.h"

#include "IGui.h"
#include "IInput.h"
#include "IWindow.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "net/Capability.h"
#include "net/WorldState.h" // kWorldStatePlayerOwned flag

#include <cmath>
#include <cstdio>

namespace fl {

namespace {

// Map a normalized [0,1] map-space point into the on-screen map rect (screen-normalized [0,1]).
HudElement rectFill(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    HudElement e;
    e.type = HudElement::Type::Rect;
    e.x = x0;
    e.y = y0;
    e.x2 = x1;
    e.y2 = y1;
    e.r = r;
    e.g = g;
    e.b = b;
    e.a = a;
    return e;
}

HudElement line(float x0, float y0, float x1, float y1, float r, float g, float b, float a, float w = 1.f) {
    HudElement e;
    e.type = HudElement::Type::Line;
    e.x = x0;
    e.y = y0;
    e.x2 = x1;
    e.y2 = y1;
    e.strokeWidth = w;
    e.r = r;
    e.g = g;
    e.b = b;
    e.a = a;
    return e;
}

// Faction colour for an entity's marker: own faction green, others red/amber/grey. The GM sees a
// simple three-way — the map is a battlespace picture, not the pilot IFF (which is #688's job).
void factionColor(uint16_t faction, uint16_t ownFaction, float& r, float& g, float& b) {
    if (faction != 0 && faction == ownFaction) {
        r = 0.2f;
        g = 1.0f;
        b = 0.3f; // friendly green
    } else if (faction == 0) {
        r = 0.7f;
        g = 0.7f;
        b = 0.7f; // neutral grey
    } else {
        r = 1.0f;
        g = 0.3f;
        b = 0.2f; // hostile red
    }
}

} // namespace

void GmMapOverlay::setOpen(bool open) noexcept {
    m_open = open;
    if (!open) {
        m_elements.clear();
        m_labelStore.clear();
    }
}

void GmMapOverlay::sendOrder(std::string_view cmd) {
    if (m_deps.serverCommand)
        m_deps.serverCommand(cmd);
}

void GmMapOverlay::update(IInput& input, IWindow& window) {
    if (!m_open)
        return;

    const int logicalW = window.logicalWidth() > 0 ? window.logicalWidth() : 1;
    const int logicalH = window.logicalHeight() > 0 ? window.logicalHeight() : 1;

    // The map rect's aspect (in pixels), so a metre is isotropic on screen and headings look right.
    const float mapWpx = (kMapX1 - kMapX0) * static_cast<float>(logicalW);
    const float mapHpx = (kMapY1 - kMapY0) * static_cast<float>(logicalH);
    m_view.aspect = mapHpx > 0.f ? mapWpx / mapHpx : 1.f;

    // Pan (arrow keys / WASD-free to avoid flight clash: use bracket + arrows) and zoom (+/-). Pan is
    // in world metres proportional to the current span so it feels the same at any zoom.
    const double panStep = m_view.spanMetresY * 0.04;
    if (input.isKeyDown(Key::ArrowLeft))
        m_view.pan(-panStep, 0.0);
    if (input.isKeyDown(Key::ArrowRight))
        m_view.pan(panStep, 0.0);
    if (input.isKeyDown(Key::ArrowUp))
        m_view.pan(0.0, -panStep);
    if (input.isKeyDown(Key::ArrowDown))
        m_view.pan(0.0, panStep);
    if (input.isKeyJustPressed(Key::Equals))
        m_view.zoom(0.8);
    if (input.isKeyJustPressed(Key::Minus))
        m_view.zoom(1.25);

    const GmMapView savedForPick = m_view;

    // Click-select: only when the click is inside the map rect and not captured by the IGui panel.
    const bool guiWantsMouse = m_deps.gui && m_deps.gui->wantCaptureMouse();
    if (!guiWantsMouse && input.isMouseButtonJustPressed(MouseButton::Left) && m_deps.net) {
        int mx = 0, my = 0;
        input.getMousePosition(mx, my);
        const float nxWin = static_cast<float>(mx) / static_cast<float>(logicalW);
        const float nyWin = static_cast<float>(my) / static_cast<float>(logicalH);
        if (nxWin >= kMapX0 && nxWin <= kMapX1 && nyWin >= kMapY0 && nyWin <= kMapY1) {
            // Window-normalized -> map-normalized [0,1] within the map rect.
            const float mapNx = (nxWin - kMapX0) / (kMapX1 - kMapX0);
            const float mapNy = (nyWin - kMapY0) / (kMapY1 - kMapY0);
            const auto& feed = m_deps.net->gmWorldState();
            const int hit = savedForPick.pick(feed.entities, glm::vec2{mapNx, mapNy}, 0.03f);
            if (hit >= 0) {
                m_hasSel = true;
                m_selIdx = feed.entities[static_cast<std::size_t>(hit)].entityIdx;
                m_selGen = feed.entities[static_cast<std::size_t>(hit)].gen;
            } else {
                m_hasSel = false;
            }
        }
    }

    rebuildElements(logicalW, logicalH);
    renderPanel();
}

void GmMapOverlay::rebuildElements(int /*logicalW*/, int /*logicalH*/) {
    m_elements.clear();
    m_labelStore.clear();
    if (!m_deps.net)
        return;

    const auto& feed = m_deps.net->gmWorldState();
    const uint16_t ownFaction = m_deps.net->ownFactionIndex();

    // Reserve label storage up front so std::string reallocation never dangles a string_view.
    m_labelStore.reserve(feed.entities.size() + 4);

    // Map background + border.
    m_elements.push_back(rectFill(kMapX0, kMapY0, kMapX1, kMapY1, 0.03f, 0.05f, 0.07f, 0.85f));
    m_elements.push_back(line(kMapX0, kMapY0, kMapX1, kMapY0, 0.3f, 0.5f, 0.4f, 0.8f));
    m_elements.push_back(line(kMapX0, kMapY1, kMapX1, kMapY1, 0.3f, 0.5f, 0.4f, 0.8f));
    m_elements.push_back(line(kMapX0, kMapY0, kMapX0, kMapY1, 0.3f, 0.5f, 0.4f, 0.8f));
    m_elements.push_back(line(kMapX1, kMapY0, kMapX1, kMapY1, 0.3f, 0.5f, 0.4f, 0.8f));

    // A light grid (quarter divisions) for scale reference.
    for (int i = 1; i < 4; ++i) {
        const float fx = kMapX0 + (kMapX1 - kMapX0) * static_cast<float>(i) / 4.f;
        const float fy = kMapY0 + (kMapY1 - kMapY0) * static_cast<float>(i) / 4.f;
        m_elements.push_back(line(fx, kMapY0, fx, kMapY1, 0.15f, 0.25f, 0.2f, 0.5f));
        m_elements.push_back(line(kMapX0, fy, kMapX1, fy, 0.15f, 0.25f, 0.2f, 0.5f));
    }

    // Title + zoom readout.
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "GM MAP  span %.0f km", m_view.spanMetresY / 1000.0);
        m_labelStore.emplace_back(buf);
        HudElement t;
        t.type = HudElement::Type::Text;
        t.x = kMapX0 + 0.005f;
        t.y = kMapY0 + 0.005f;
        t.r = 0.5f;
        t.g = 1.0f;
        t.b = 0.6f;
        t.scale = 1.0f;
        t.text = m_labelStore.back();
        m_elements.push_back(t);
    }

    const float mapW = kMapX1 - kMapX0;
    const float mapH = kMapY1 - kMapY0;

    auto toScreen = [&](float mapNx, float mapNy, float& sx, float& sy) {
        sx = kMapX0 + mapNx * mapW;
        sy = kMapY0 + mapNy * mapH;
    };

    for (const auto& e : feed.entities) {
        const glm::vec2 mp = m_view.worldToMap(e.pos[0], e.pos[2]);
        if (mp.x < 0.f || mp.x > 1.f || mp.y < 0.f || mp.y > 1.f)
            continue; // off the current view
        float sx = 0.f, sy = 0.f;
        toScreen(mp.x, mp.y, sx, sy);
        float r = 1.f, g = 1.f, b = 1.f;
        factionColor(e.factionIndex, ownFaction, r, g, b);

        // Marker: a small filled square (a few px, expressed as a small normalized box).
        const float hs = 0.006f;
        m_elements.push_back(
            rectFill(sx - hs, sy - hs * m_view.aspect, sx + hs, sy + hs * m_view.aspect, r, g, b, 1.f));

        // Heading vector from velXZ.
        const float vlen = std::sqrt(e.velXZ[0] * e.velXZ[0] + e.velXZ[1] * e.velXZ[1]);
        if (vlen > 1.f) {
            const float dx = e.velXZ[0] / vlen * 0.02f;
            const float dy = e.velXZ[1] / vlen * 0.02f * m_view.aspect;
            m_elements.push_back(line(sx, sy, sx + dx, sy + dy, r, g, b, 0.9f, 1.5f));
        }

        // Label player-owned entities with their callsign.
        if ((e.flags & kWorldStatePlayerOwned) && e.ownerPeerId != 0) {
            const std::string name = m_deps.net->displayName(e.ownerPeerId);
            if (!name.empty()) {
                m_labelStore.emplace_back(name);
                HudElement t;
                t.type = HudElement::Type::Text;
                t.x = sx + 0.008f;
                t.y = sy - 0.008f;
                t.r = r;
                t.g = g;
                t.b = b;
                t.scale = 0.75f;
                t.text = m_labelStore.back();
                m_elements.push_back(t);
            }
        }
    }

    // Selection box (resolve the {idx,gen} against the live feed; a stale pick just draws nothing).
    if (m_hasSel) {
        for (const auto& e : feed.entities) {
            if (e.entityIdx != m_selIdx || e.gen != m_selGen)
                continue;
            const glm::vec2 mp = m_view.worldToMap(e.pos[0], e.pos[2]);
            if (mp.x < 0.f || mp.x > 1.f || mp.y < 0.f || mp.y > 1.f)
                break;
            float sx = 0.f, sy = 0.f;
            toScreen(mp.x, mp.y, sx, sy);
            const float hs = 0.014f;
            const float hy = hs * m_view.aspect;
            m_elements.push_back(line(sx - hs, sy - hy, sx + hs, sy - hy, 1.f, 1.f, 0.2f, 1.f, 1.5f));
            m_elements.push_back(line(sx - hs, sy + hy, sx + hs, sy + hy, 1.f, 1.f, 0.2f, 1.f, 1.5f));
            m_elements.push_back(line(sx - hs, sy - hy, sx - hs, sy + hy, 1.f, 1.f, 0.2f, 1.f, 1.5f));
            m_elements.push_back(line(sx + hs, sy - hy, sx + hs, sy + hy, 1.f, 1.f, 0.2f, 1.f, 1.5f));
            break;
        }
    }
}

void GmMapOverlay::renderPanel() {
    IGui* gui = m_deps.gui;
    if (!gui)
        return;
    if (!gui->beginWindow("Game Master", 0.76f, 0.06f, 0.22f, 0.90f))
        return;

    const bool authorized = m_deps.net && m_deps.net->hasCapability(Capability::GmMap);
    if (!authorized)
        gui->label("(no game-master authority -- view only)");

    // Resolve the selected entity from the live feed.
    const GmEntityRecord* sel = nullptr;
    if (m_hasSel && m_deps.net) {
        for (const auto& e : m_deps.net->gmWorldState().entities) {
            if (e.entityIdx == m_selIdx && e.gen == m_selGen) {
                sel = &e;
                break;
            }
        }
    }

    if (!sel) {
        gui->label("No entity selected.");
        gui->label("Click a marker on the map.");
        gui->endWindow();
        return;
    }

    // Details.
    char buf[96];
    const char* typeName = "?";
    if (m_deps.registry) {
        if (const EntityDef* def = m_deps.registry->byIndex(sel->typeIndex))
            typeName = def->name.empty() ? def->id.c_str() : def->name.c_str();
    }
    std::snprintf(buf, sizeof(buf), "Entity %u  (%s)", sel->entityIdx, typeName);
    gui->label(buf);
    const std::string facName = m_deps.net ? m_deps.net->factionName(sel->factionIndex) : std::string{};
    std::snprintf(buf, sizeof(buf), "Faction: %s", facName.empty() ? "?" : facName.c_str());
    gui->label(buf);
    std::snprintf(buf, sizeof(buf), "HP: %u%%   %s", sel->hpPct, sel->formationId ? "in flight" : "no flight");
    gui->label(buf);
    gui->separator();

    // View-from-entity (always available — spectating any entity needs no order authority beyond the
    // GM map, and the server gates the spectate command).
    if (gui->button("View from here")) {
        m_viewRequest = ViewRequest{sel->entityIdx, sel->gen};
        // Ask the server to centre this peer's interest on the target so it streams into the client's
        // snapshot even if it was far outside the observer's camera interest (the #403 spectate seam).
        if (m_deps.net) {
            char sb[64];
            std::snprintf(sb, sizeof(sb), "spectate %u %u", m_deps.net->selfPeerId(), sel->entityIdx);
            sendOrder(sb);
        }
        setOpen(false);
    }

    if (!authorized) {
        gui->endWindow();
        return;
    }

    gui->separator();
    // Orders. An AI entity already in a flight can be ordered with the six-command grammar; a bare AI
    // entity can be formed into a flight first (its formationId then appears on the next 1 Hz update).
    const bool isAi = (sel->flags & kWorldStatePlayerOwned) == 0;
    if (sel->formationId != 0) {
        gui->label("Order flight:");
        if (gui->button("Engage bandits")) {
            std::snprintf(buf, sizeof(buf), "flight order %u engage_bandits", sel->formationId);
            sendOrder(buf);
        }
        if (gui->button("Rejoin")) {
            std::snprintf(buf, sizeof(buf), "flight order %u rejoin", sel->formationId);
            sendOrder(buf);
        }
        if (gui->button("Cover me")) {
            std::snprintf(buf, sizeof(buf), "flight order %u cover_me", sel->formationId);
            sendOrder(buf);
        }
        if (gui->button("Hold fire")) {
            std::snprintf(buf, sizeof(buf), "flight order %u hold_fire", sel->formationId);
            sendOrder(buf);
        }
        if (gui->button("Return to base")) {
            std::snprintf(buf, sizeof(buf), "flight order %u return_to_base", sel->formationId);
            sendOrder(buf);
        }
    } else if (isAi) {
        if (gui->button("Form flight on this entity")) {
            std::snprintf(buf, sizeof(buf), "flight create %u", sel->entityIdx);
            sendOrder(buf);
        }
    }

    gui->separator();
    if (gui->button("Destroy entity")) {
        std::snprintf(buf, sizeof(buf), "kill %u", sel->entityIdx);
        sendOrder(buf);
        m_hasSel = false;
    }

    gui->endWindow();
}

} // namespace fl
