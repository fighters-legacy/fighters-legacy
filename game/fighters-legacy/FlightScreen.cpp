// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlightScreen.h"

#include "CameraInput.h"
#include "ClientNetEventHandler.h"
#include "ClientPrediction.h"
#include "FlightInputCollector.h"
#include "HapticController.h"
#include "IInput.h"
#include "INetwork.h"
#include "IWindow.h"
#include "ManualOverlay.h"
#include "config/ControlsSettings.h"
#include "config/UserConfig.h"
#include "console/GameConsole.h"
#include "flight/Geodetic.h"   // kEarthRadiusM
#include "flight/LocalFrame.h" // bankOf on the local-level frame
#include "render/CameraController.h"
#include "render/FlightHud.h"
#include "render/IHud.h"
#include "render/SimRenderBridge.h"
#include "render/TerrainStreamer.h"
#include "render/WindshieldRain.h"
#include "sandbox/SandboxInspector.h"

#include <cmath>
#include <glm/glm.hpp>

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
        d.clientNetHandler->snapshotCallback =
            [pred = d.prediction, env = d.env](RenderSnapshot& snap, uint64_t tickIndex, uint32_t delayTicks) {
                pred->reconcile(snap, tickIndex, delayTicks, *env);
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

    d.camInput->pollModeKeys(*d.cameraController, *d.gameConsole, input, m_playerEntry);
    d.camInput->update(*d.cameraController, m_playerEntry, *d.gameConsole, *d.terrainStreamer);

    // Radio menu (#610). Non-modal: the aircraft keeps flying while it is open (see WingmanMenu.h),
    // so only the discrete keys it consumes are suppressed, via FlightInputCollector's uiFocused.
    const bool menuWasOpen = d.wingmanMenu && d.wingmanMenu->isOpen();
    if (d.wingmanMenu && !d.gameConsole->isOpen()) {
        if (!menuWasOpen && input.isKeyJustPressed(Key::C)) {
            d.wingmanMenu->toggle();
        } else if (menuWasOpen) {
            if (auto order = d.wingmanMenu->update(input); order && d.clientNet) {
                d.clientNet->send(fl::kNetChReliable, &*order, sizeof(*order), /*reliable=*/true);
            }
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
    const bool uiFocused = d.wingmanMenu && d.wingmanMenu->isOpen();
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
    const float terrainElev =
        m_playerEntry ? static_cast<float>(d.terrainStreamer->heightAt(m_playerEntry->position)) : 0.f;
    const bool cockpit = (d.cameraController->mode() == fl::CameraMode::Cockpit);

    static constexpr uint32_t kMinLatencyDisplayMs = 5u;
    const uint32_t latencyMs = d.clientNetHandler ? d.clientNetHandler->snapshotLatencyMs() : 0u;
    const bool showLat = d.userConfig->hud().showLatency && d.clientNetHandler &&
                         d.clientNetHandler->hasSnapshotLatency() && latencyMs >= kMinLatencyDisplayMs;

    // Planet radius (m) from the server's MsgConnectAck; drives the local-level HUD attitude/horizon
    // and windshield lean. Earth default until the ack arrives.
    const double radiusM =
        d.clientNetHandler ? static_cast<double>(d.clientNetHandler->planetRadiusKm()) * 1000.0 : fl::kEarthRadiusM;

    (*d.activeHud)
        ->update(cockpit ? m_playerEntry : nullptr, d.env->timeOfDay, terrainElev, latencyMs, showLat, radiusM);
    d.windshieldRain->update(cockpit ? (1.f / 60.f) : 0.f, cockpit ? *d.env : EnvironmentState{},
                             cockpit ? rollAngleRad(m_playerEntry, radiusM) : 0.f);
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
    if (!menuWasOpen && !manualClosedThisFrame && !consoleWasOpen && !d.gameConsole->isOpen() &&
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
    if (m_deps.manual && m_deps.manual->isOpen()) {
        for (const auto& e : m_deps.manual->elements()) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }
    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}

} // namespace fl