// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlightScreen.h"

#include "CameraInput.h"
#include "FlightInputCollector.h"
#include "HapticController.h"
#include "IInput.h"
#include "INetwork.h"
#include "IWindow.h"
#include "config/ControlsSettings.h"
#include "config/UserConfig.h"
#include "console/GameConsole.h"
#include "render/CameraController.h"
#include "render/FlightHud.h"
#include "render/IHud.h"
#include "render/SimRenderBridge.h"
#include "render/TerrainStreamer.h"
#include "render/WindshieldRain.h"
#include "sandbox/SandboxInspector.h"

#include <cmath>
#include <glm/glm.hpp>

static constexpr float kSnowAltitude = 2000.0f;

static const fl::EntityRenderEntry* findEntry(const fl::SimRenderBridge& bridge, uint32_t idx, uint32_t gen) {
    if (!bridge.hasSnapshot())
        return nullptr;
    for (const auto& e : bridge.current().entries)
        if (e.entityIdx == idx && e.entityGen == gen)
            return &e;
    return nullptr;
}

static float rollAngleRad(const fl::EntityRenderEntry* p) {
    if (!p)
        return 0.f;
    const glm::vec3 up = p->orientation * glm::vec3(0.f, 1.f, 0.f);
    const glm::vec3 right = p->orientation * glm::vec3(0.f, 0.f, 1.f);
    return std::atan2(-right.y, up.y);
}

FlightScreen::FlightScreen(FlightScreenDeps deps) : m_deps(std::move(deps)) {}

Screen FlightScreen::update(IInput& input, IWindow& /*window*/) {
    auto& d = m_deps;

    uint32_t idx = d.assignedEntityIdx ? *d.assignedEntityIdx : 0;
    uint32_t gen = d.assignedEntityGen ? *d.assignedEntityGen : 0;
    m_playerEntry = findEntry(*d.renderBridge, idx, gen);

    d.camInput->pollModeKeys(*d.cameraController, *d.gameConsole, input, m_playerEntry);
    d.camInput->update(*d.cameraController, m_playerEntry, *d.gameConsole, *d.terrainStreamer);

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
    if (auto msg = d.flightInput->poll(*d.renderBridge, *d.camInput, *d.gameConsole, input, d.joystick, cs))
        d.clientNet->send(0, &*msg, sizeof(*msg), /*reliable=*/false);
    m_weaponFired = d.flightInput->wasWeaponFired();

    const float terrainElev =
        m_playerEntry
            ? static_cast<float>(d.terrainStreamer->heightAt(m_playerEntry->position.x, m_playerEntry->position.z))
            : 0.f;
    const bool cockpit = (d.cameraController->mode() == fl::CameraMode::Cockpit);
    const bool isSnow = m_playerEntry && static_cast<float>(m_playerEntry->position.y) > kSnowAltitude;

    (*d.activeHud)->update(cockpit ? m_playerEntry : nullptr, d.env->timeOfDay, terrainElev);
    d.windshieldRain->update(cockpit ? (1.f / 60.f) : 0.f, cockpit ? *d.env : EnvironmentState{},
                             cockpit ? rollAngleRad(m_playerEntry) : 0.f, cockpit && isSnow);
    if (d.hapticController)
        d.hapticController->update(m_playerEntry, m_weaponFired, terrainElev, 1.f / 60.f);

    if (!consoleWasOpen && !d.gameConsole->isOpen() && input.isKeyJustPressed(Key::Escape))
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
    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}
