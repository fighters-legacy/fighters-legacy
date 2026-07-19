// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlightScreen.h"

#include "CameraInput.h"
#include "ClientNetEventHandler.h"
#include "ClientPrediction.h"
#include "CommsMenu.h"
#include "FlightInputCollector.h"
#include "HapticController.h"
#include "IInput.h"
#include "INetwork.h"
#include "IWindow.h"
#include "ManualOverlay.h"
#include "config/ControlsSettings.h"
#include "config/UserConfig.h"
#include "console/GameConsole.h"
#include "entity/EntityDef.h"          // EntityDef::name/id for the observer picker label (#860)
#include "entity/EntityTypeRegistry.h" // byIndex
#include "flight/Geodetic.h"           // kEarthRadiusM
#include "flight/LocalFrame.h"         // bankOf on the local-level frame
#include "render/CameraController.h"
#include "render/FlightHud.h"
#include "render/IHud.h"
#include "render/SimRenderBridge.h"
#include "render/TerrainStreamer.h"
#include "render/WindshieldRain.h"
#include "sandbox/SandboxInspector.h"

#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>
#include <span>
#include <string>

namespace fl {

static const fl::EntityRenderEntry* findEntry(const fl::SimRenderBridge& bridge, uint32_t idx, uint32_t gen) {
    if (!bridge.hasSnapshot())
        return nullptr;
    for (const auto& e : bridge.current().entries)
        if (e.entityIdx == idx && e.entityGen == gen)
            return &e;
    return nullptr;
}

static float rollAngleRad(const fl::EntityRenderEntry* p, double planetRadiusM) {
    if (!p)
        return 0.f;
    // Bank relative to the LOCAL up (radial on a spherical planet), so the windshield lean stays
    // correct far from the world origin (#479). Reduces to atan2(-right.y, up.y) near the origin.
    const float q[4] = {p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w};
    return fl::bankOf(q, p->position, planetRadiusM);
}

FlightScreen::FlightScreen(FlightScreenDeps deps) : m_deps(std::move(deps)) {
    auto& d = m_deps;
    if (d.clientNetHandler && d.prediction && d.env) {
        d.clientNetHandler->snapshotCallback = [pred = d.prediction,
                                                env = d.env](RenderSnapshot& snap, uint64_t tickIndex,
                                                             uint32_t delayTicks, uint32_t ackedSeqNum) {
            pred->reconcile(snap, tickIndex, delayTicks, ackedSeqNum, *env);
        };
    }
}

FlightScreen::~FlightScreen() {
    if (m_deps.clientNetHandler)
        m_deps.clientNetHandler->snapshotCallback = nullptr;
}

Screen FlightScreen::update(IInput& input, IWindow& /*window*/) {
    auto& d = m_deps;

    uint32_t idx = d.assignedEntityIdx ? *d.assignedEntityIdx : 0;
    uint32_t gen = d.assignedEntityGen ? *d.assignedEntityGen : 0;
    m_playerEntry = findEntry(*d.renderBridge, idx, gen);

    // Observer entity picker (#860): a spectator has no ownship, so the camera views a SELECTED live
    // entity. Num1/Num2 cycle the selection; the first pick jumps the free ghost camera into Chase so
    // the choice is immediately visible. A pilot always views its own aircraft.
    const bool observer = d.clientNetHandler && d.clientNetHandler->grantedRole() == fl::PeerRole::Observer;
    const fl::EntityRenderEntry* viewEntry = m_playerEntry;
    if (observer) {
        const std::span<const fl::EntityRenderEntry> entries =
            d.renderBridge->hasSnapshot() ? std::span<const fl::EntityRenderEntry>(d.renderBridge->current().entries)
                                          : std::span<const fl::EntityRenderEntry>{};
        if (!d.gameConsole->isOpen() && !(d.wingmanMenu && d.wingmanMenu->isOpen())) {
            const bool nextDown = input.isKeyDown(Key::Num1);
            const bool prevDown = input.isKeyDown(Key::Num2);
            if ((nextDown && !m_prevNextTarget) || (prevDown && !m_prevPrevTarget)) {
                if (nextDown && !m_prevNextTarget)
                    m_selector.cycleNext(entries);
                else
                    m_selector.cyclePrev(entries);
                if (d.cameraController->mode() == fl::CameraMode::Free)
                    d.cameraController->setMode(fl::CameraMode::Chase);
            }
            m_prevNextTarget = nextDown;
            m_prevPrevTarget = prevDown;
        }
        viewEntry = m_selector.resolve(entries);
        // Graceful degrade: the picked entity is gone (destroyed / interest-out / pool-slot reuse), so
        // drop back to the free ghost camera rather than freezing on an empty Chase/Cockpit.
        if (!viewEntry && d.cameraController->mode() != fl::CameraMode::Free)
            d.cameraController->setMode(fl::CameraMode::Free);
    }

    d.camInput->pollModeKeys(*d.cameraController, *d.gameConsole, input, viewEntry);
    d.camInput->update(*d.cameraController, viewEntry, *d.gameConsole, *d.terrainStreamer);

    // Radio menu (#610). Non-modal: the aircraft keeps flying while it is open (see WingmanMenu.h),
    // so only the discrete keys it consumes are suppressed, via FlightInputCollector's uiFocused.
    const bool menuWasOpen = d.wingmanMenu && d.wingmanMenu->isOpen();
    const bool commsMenuWasOpen = d.commsMenu && d.commsMenu->isOpen();
    if (d.wingmanMenu && !d.gameConsole->isOpen() && !commsMenuWasOpen) {
        if (!menuWasOpen && input.isKeyJustPressed(Key::C)) {
            d.wingmanMenu->toggle();
        } else if (menuWasOpen) {
            if (auto order = d.wingmanMenu->update(input); order && d.clientNet) {
                d.clientNet->send(fl::kNetChReliable, &*order, sizeof(*order), /*reliable=*/true);
            }
        }
    }

    // ATC comms menu (#704). Same non-modal contract as the wingman menu — the aircraft keeps flying;
    // only the discrete keys it consumes are suppressed via uiFocused. T toggles it. Mutually exclusive
    // with the wingman menu so their digit keys never collide.
    if (d.commsMenu && !d.gameConsole->isOpen() && !menuWasOpen) {
        if (!commsMenuWasOpen && input.isKeyJustPressed(Key::T)) {
            d.commsMenu->toggle();
        } else if (commsMenuWasOpen) {
            if (auto cmd = d.commsMenu->update(input); cmd && d.clientNet) {
                d.clientNet->send(fl::kNetChReliable, &*cmd, sizeof(*cmd), /*reliable=*/true);
            }
        }
    }

    // Crew seat picker (#975), non-modal like the radio menu. K cycles joinable seats across every
    // crewed aircraft the client knows; L joins the selected seat; U leaves the current seat. These keys
    // are NOT flight controls (avoiding J = ECM etc.), so no input is suppressed. Suppressed only while
    // the console/radio menu is up (they own the keyboard then).
    if (d.clientNetHandler && !d.gameConsole->isOpen() && !(d.wingmanMenu && d.wingmanMenu->isOpen())) {
        const bool kNow = input.isKeyDown(Key::K), lNow = input.isKeyDown(Key::L), uNow = input.isKeyDown(Key::U);
        if (kNow && !m_prevSeatCycle) {
            m_seatPicker.rebuild(d.clientNetHandler->crewRosters());
            if (!m_seatPickerActive)
                m_seatPickerActive = true; // first press opens the overlay
            else
                m_seatPicker.next();
        }
        if (lNow && !m_prevSeatJoin && m_seatPickerActive) {
            if (const fl::SeatTarget* t = m_seatPicker.selected())
                d.clientNetHandler->sendSeatRequest(t->entityIdx, t->entityGen, t->seatIndex);
        }
        if (uNow && !m_prevSeatLeave)
            d.clientNetHandler->sendSeatLeave();
        m_prevSeatCycle = kNow;
        m_prevSeatJoin = lNow;
        m_prevSeatLeave = uNow;

        // Surface the last MsgSeatResult as a one-line label (client-side strings, localizable).
        if (const auto res = d.clientNetHandler->takeSeatResult(); res.fresh) {
            const char* msg = "seat: ?";
            switch (static_cast<fl::SeatResultCode>(res.code)) {
            case fl::SeatResultCode::Granted:
                msg = res.entityIdx ? "seat: joined" : "seat: left";
                m_seatPickerActive = false;
                break;
            case fl::SeatResultCode::SeatOccupiedByHuman:
                msg = "seat: taken by another player";
                break;
            case fl::SeatResultCode::FlySeatNotJoinable:
                msg = "seat: pilot seat not joinable";
                break;
            case fl::SeatResultCode::NotCrewed:
                msg = "seat: not a crewed aircraft";
                break;
            case fl::SeatResultCode::NoSuchSeat:
                msg = "seat: no such seat";
                break;
            case fl::SeatResultCode::NoSuchEntity:
                msg = "seat: aircraft gone";
                break;
            case fl::SeatResultCode::NotInSeat:
                msg = "seat: you hold no seat";
                break;
            }
            std::snprintf(m_seatResultLine, sizeof(m_seatResultLine), "%s", msg);
        }
    }

    const bool consoleWasOpen = d.gameConsole->isOpen();
    if (consoleWasOpen) {
        if (d.gameConsole->tick(input))
            d.gameConsole->close(input);
    }
    if (!consoleWasOpen && d.gameConsole->isOpen() && d.hapticController)
        d.hapticController->onPause(0);

    // SandboxInspector intercepts Escape; returning false = user requested exit.
    if (d.inspector && !d.inspector->update() && !consoleWasOpen)
        return Screen::MainMenu;

    const ControlsSettings cs = d.userConfig->controls();
    const bool uiFocused = (d.wingmanMenu && d.wingmanMenu->isOpen()) || (d.commsMenu && d.commsMenu->isOpen());
    if (auto msg =
            d.flightInput->poll(*d.renderBridge, *d.camInput, *d.gameConsole, input, d.joystick, cs, uiFocused)) {
        // Stamp the snapshot ack (tickIndex + selective-ack mask, #566) from the net handler — the single
        // ack authority — before prediction and send, so the outgoing input carries a consistent ack.
        if (d.clientNetHandler)
            d.clientNetHandler->stampAck(*msg);
        if (d.prediction && d.env)
            d.prediction->onInput(*msg, *d.env);
        d.clientNet->send(0, &*msg, sizeof(*msg), /*reliable=*/false);
    }
    if (d.clientNetHandler)
        d.clientNetHandler->sendHeartbeatIfNeeded();
    m_weaponFired = d.flightInput->wasWeaponFired();

    // Terrain elevation above the datum along the radial through the entity (heightAt(dvec3));
    // the HUD/haptics derive radial AGL from it against the geodetic altitude (#477).
    const float terrainElev = viewEntry ? static_cast<float>(d.terrainStreamer->heightAt(viewEntry->position)) : 0.f;
    const bool cockpit = (d.cameraController->mode() == fl::CameraMode::Cockpit);

    // Observer picker label (#860): name + faction of the entity being viewed, shown top-centre. Built
    // here where the registry (type name) and the net handler (faction name) are both reachable.
    m_pickerLabel[0] = '\0';
    if (observer && viewEntry) {
        const char* typeName = "entity";
        if (d.entityRegistry) {
            if (const fl::EntityDef* def = d.entityRegistry->byIndex(viewEntry->typeIndex))
                typeName = def->name.empty() ? def->id.c_str() : def->name.c_str();
        }
        const std::string faction =
            d.clientNetHandler ? d.clientNetHandler->factionName(viewEntry->factionIndex) : std::string{};
        if (!faction.empty())
            std::snprintf(m_pickerLabel, sizeof(m_pickerLabel), "[ %s  |  %s ]", typeName, faction.c_str());
        else
            std::snprintf(m_pickerLabel, sizeof(m_pickerLabel), "[ %s ]", typeName);
    }

    static constexpr uint32_t kMinLatencyDisplayMs = 5u;
    const uint32_t latencyMs = d.clientNetHandler ? d.clientNetHandler->snapshotLatencyMs() : 0u;
    const bool showLat = d.userConfig->hud().showLatency && d.clientNetHandler &&
                         d.clientNetHandler->hasSnapshotLatency() && latencyMs >= kMinLatencyDisplayMs;

    // Planet radius (m) from the server's MsgConnectAck; drives the local-level HUD attitude/horizon
    // and windshield lean. Earth default until the ack arrives.
    const double radiusM =
        d.clientNetHandler ? static_cast<double>(d.clientNetHandler->planetRadiusKm()) * 1000.0 : fl::kEarthRadiusM;

    // Datalink track picture + RWR (#528) for the HUD radar scope — only when this peer flies its own
    // aircraft (a spectator following another entity has no datalink of its own to draw).
    const fl::RadarView radar = (cockpit && d.clientNetHandler) ? d.clientNetHandler->radarView() : fl::RadarView{};
    (*d.activeHud)
        ->update(cockpit ? viewEntry : nullptr, d.env->timeOfDay, terrainElev, latencyMs, showLat, radiusM, radar);
    d.windshieldRain->update(cockpit ? (1.f / 60.f) : 0.f, cockpit ? *d.env : EnvironmentState{},
                             cockpit ? rollAngleRad(viewEntry, radiusM) : 0.f);
    // Haptics only for a real ownship — an observer viewing another entity should not feel its hits.
    if (d.hapticController)
        d.hapticController->update(m_playerEntry, m_weaponFired, terrainElev, 1.f / 60.f, radiusM);

    // The in-flight aircraft manual (#821). Non-modal, like the radio menu: the aircraft keeps flying
    // while you read it, because a reference you must stop flying to consult is one you never open.
    const bool manualWasOpen = d.manual && d.manual->isOpen();
    if (d.manual && !d.gameConsole->isOpen() && !menuWasOpen) {
        if (input.isKeyJustPressed(Key::M))
            d.manual->toggle();
        if (d.manual->isOpen()) {
            if (input.isKeyJustPressed(Key::PageDown))
                d.manual->scroll(+10);
            if (input.isKeyJustPressed(Key::PageUp))
                d.manual->scroll(-10);
            if (input.isKeyJustPressed(Key::Escape))
                d.manual->close();
        }
        d.manual->update();
    }

    // Escape closed the radio menu or the manual this frame; it must NOT also open the pause screen.
    const bool manualClosedThisFrame = manualWasOpen && d.manual && !d.manual->isOpen();
    if (!menuWasOpen && !commsMenuWasOpen && !manualClosedThisFrame && !consoleWasOpen && !d.gameConsole->isOpen() &&
        input.isKeyJustPressed(Key::Escape))
        return Screen::Pause;

    return Screen::Flight;
}

std::span<const HudElement> FlightScreen::buildElements() {
    m_elementCount = 0;
    const auto hudSpan = (*m_deps.activeHud)->elements();
    const auto rainSpan = m_deps.windshieldRain->elements();

    for (const auto& e : hudSpan) {
        if (m_elementCount >= kMaxElements)
            break;
        m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
    }
    for (const auto& e : rainSpan) {
        if (m_elementCount >= kMaxElements)
            break;
        m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
    }
    if (m_deps.wingmanMenu) {
        for (const auto& e : m_deps.wingmanMenu->buildElements()) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }
    if (m_deps.commsMenu) {
        for (const auto& e : m_deps.commsMenu->buildElements()) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }
    if (m_deps.manual && m_deps.manual->isOpen()) {
        for (const auto& e : m_deps.manual->elements()) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }
    // Observer picker label (#860): name + faction of the entity being viewed, top-centre.
    if (m_pickerLabel[0] && m_elementCount < kMaxElements) {
        HudElement& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = {};
        el.type = HudElement::Type::Text;
        el.x = 0.5f;
        el.y = 0.04f;
        el.align = HudAlign::Center;
        el.r = 0.6f;
        el.g = 0.9f;
        el.b = 1.0f;
        el.a = 1.0f;
        el.scale = 1.f;
        el.text = m_pickerLabel;
    }
    // Crew seat picker overlay (#975): the current selection + a hint, bottom-left. Rebuilt each frame
    // into a member buffer (HudElement::text is non-owning).
    auto addLine = [&](const char* txt, float y, float g) {
        if (!txt[0] || m_elementCount >= kMaxElements)
            return;
        HudElement& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = {};
        el.type = HudElement::Type::Text;
        el.x = 0.02f;
        el.y = y;
        el.align = HudAlign::Left;
        el.r = 0.7f;
        el.g = g;
        el.b = 0.5f;
        el.a = 1.0f;
        el.scale = 1.f;
        el.text = txt;
    };
    if (m_seatPickerActive) {
        if (const fl::SeatTarget* t = m_seatPicker.selected())
            std::snprintf(m_seatPickerLine, sizeof(m_seatPickerLine), "SEAT: %s @%u  [K next  L join  U leave]",
                          t->role.c_str(), t->entityIdx);
        else
            std::snprintf(m_seatPickerLine, sizeof(m_seatPickerLine), "SEAT: no free crew seats");
        addLine(m_seatPickerLine, 0.86f, 0.9f);
    }
    addLine(m_seatResultLine, 0.90f, 0.9f);
    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}

} // namespace fl