// SPDX-License-Identifier: GPL-3.0-or-later
#include "SettingsScreen.h"

#include "IDisplay.h"
#include "IInput.h"
#include "IRenderer.h"
#include "IWindow.h"
#include "config/UserConfig.h"

#include <algorithm>
#include <cmath>
#include <string>

static float drawDistKm(DrawDistance d) {
    switch (d) {
    case DrawDistance::Low:
        return 20.0f;
    case DrawDistance::Medium:
        return 50.0f;
    case DrawDistance::High:
        return 100.0f;
    case DrawDistance::Ultra:
        return 200.0f;
    }
    return 50.0f;
}

static const char* vsyncLabel(VsyncMode v) {
    switch (v) {
    case VsyncMode::Off:
        return "Off";
    case VsyncMode::On:
        return "On";
    case VsyncMode::Adaptive:
        return "Adaptive";
    }
    return "On";
}

static const char* drawDistLabel(DrawDistance d) {
    switch (d) {
    case DrawDistance::Low:
        return "Low (20 km)";
    case DrawDistance::Medium:
        return "Medium (50 km)";
    case DrawDistance::High:
        return "High (100 km)";
    case DrawDistance::Ultra:
        return "Ultra (200 km)";
    }
    return "High";
}

SettingsScreen::SettingsScreen(UserConfig& config, IRenderer& renderer, IWindow& window, IDisplay& display)
    : m_userConfig(config), m_renderer(renderer), m_window(window), m_display(display) {
    m_graphics = config.graphics();
    m_audio = config.audio();
    buildModes();
}

void SettingsScreen::buildModes() {
    m_modes.clear();
    int monId = m_window.getCurrentMonitorId();
    auto modes = m_display.listModes(monId);
    for (auto& dm : modes) {
        bool dup = false;
        for (auto& p : m_modes)
            if (p.first == dm.width && p.second == dm.height) {
                dup = true;
                break;
            }
        if (!dup)
            m_modes.emplace_back(dm.width, dm.height);
    }
    std::sort(m_modes.begin(), m_modes.end(),
              [](const auto& a, const auto& b) { return a.first * a.second > b.first * b.second; });

    m_modeIdx = 0;
    if (m_graphics.resolutionWidth > 0 && m_graphics.resolutionHeight > 0) {
        for (int i = 0; i < static_cast<int>(m_modes.size()); ++i) {
            if (m_modes[static_cast<std::size_t>(i)].first == m_graphics.resolutionWidth &&
                m_modes[static_cast<std::size_t>(i)].second == m_graphics.resolutionHeight) {
                m_modeIdx = i;
                break;
            }
        }
    }
}

void SettingsScreen::applyAndSave() {
    m_userConfig.setGraphics(m_graphics);
    m_userConfig.setAudio(m_audio);
    m_userConfig.save();

    RendererSettings rs;
    switch (m_graphics.vsync) {
    case VsyncMode::Off:
        rs.vsync = RendererVsyncMode::Off;
        break;
    case VsyncMode::On:
        rs.vsync = RendererVsyncMode::On;
        break;
    case VsyncMode::Adaptive:
        rs.vsync = RendererVsyncMode::Adaptive;
        break;
    }
    rs.antiAliasing = m_graphics.antiAliasing;
    rs.bloom = true; // not surfaced in Phase 2 settings
    rs.drawDistanceKm = drawDistKm(m_graphics.drawDistance);
    m_renderer.applySettings(rs);
}

Screen SettingsScreen::update(IInput& input, IWindow& window) {
    const float step = 0.05f;
    const float scrollStep = 0.01f;

    // Row navigation
    if (input.isKeyJustPressed(Key::ArrowUp) || input.isKeyJustPressed(Key::W) ||
        input.isGamepadButtonJustPressed(0, GamepadButton::DpadUp))
        m_focusedRow = (m_focusedRow - 1 + kRowCount) % kRowCount;

    if (input.isKeyJustPressed(Key::ArrowDown) || input.isKeyJustPressed(Key::S) ||
        input.isGamepadButtonJustPressed(0, GamepadButton::DpadDown))
        m_focusedRow = (m_focusedRow + 1) % kRowCount;

    // Mouse hover
    int mx = 0, my = 0;
    input.getMousePosition(mx, my);
    const float fh = static_cast<float>(window.logicalHeight());
    if (fh > 0.f) {
        const float ny = static_cast<float>(my) / fh;
        for (int r = 0; r < kRowCount; ++r) {
            float ry = 0.20f + static_cast<float>(r) * 0.07f;
            if (ny >= ry && ny < ry + 0.06f)
                m_focusedRow = r;
        }
    }

    const bool left = input.isKeyJustPressed(Key::ArrowLeft) || input.isKeyJustPressed(Key::A) ||
                      input.isGamepadButtonJustPressed(0, GamepadButton::DpadLeft);
    const bool right = input.isKeyJustPressed(Key::ArrowRight) || input.isKeyJustPressed(Key::D) ||
                       input.isGamepadButtonJustPressed(0, GamepadButton::DpadRight);
    const float scroll = static_cast<float>(input.getMouseScroll());

    switch (m_focusedRow) {
    case 0: // Resolution
        if (!m_modes.empty()) {
            if (left || scroll > 0.f)
                m_modeIdx = (m_modeIdx + 1) % static_cast<int>(m_modes.size());
            if (right || scroll < 0.f)
                m_modeIdx = (m_modeIdx - 1 + static_cast<int>(m_modes.size())) % static_cast<int>(m_modes.size());
            m_graphics.resolutionWidth = m_modes[static_cast<std::size_t>(m_modeIdx)].first;
            m_graphics.resolutionHeight = m_modes[static_cast<std::size_t>(m_modeIdx)].second;
            window.setSize(m_graphics.resolutionWidth, m_graphics.resolutionHeight);
        }
        break;
    case 1: // Display
        if (left || right || scroll != 0.f) {
            m_fullscreen = !m_fullscreen;
            window.setFullscreen(m_fullscreen);
        }
        break;
    case 2: // Vsync
        if (left || right || scroll != 0.f)
            m_graphics.vsync = static_cast<VsyncMode>((static_cast<int>(m_graphics.vsync) + 1) % 3);
        break;
    case 3: // Anti-aliasing
        if (left || right || scroll != 0.f)
            m_graphics.antiAliasing = !m_graphics.antiAliasing;
        break;
    case 4: // Draw distance
        if (left || right || scroll != 0.f)
            m_graphics.drawDistance = static_cast<DrawDistance>((static_cast<int>(m_graphics.drawDistance) + 1) % 4);
        break;
    case 5: { // Master volume
        float delta = (right ? step : 0.f) + (left ? -step : 0.f) + scrollStep * scroll;
        m_audio.masterVolume = std::clamp(m_audio.masterVolume + delta, 0.f, 1.f);
        break;
    }
    case 6: { // Music volume
        float delta = (right ? step : 0.f) + (left ? -step : 0.f) + scrollStep * scroll;
        m_audio.musicVolume = std::clamp(m_audio.musicVolume + delta, 0.f, 1.f);
        break;
    }
    case 7: { // SFX volume
        float delta = (right ? step : 0.f) + (left ? -step : 0.f) + scrollStep * scroll;
        m_audio.sfxVolume = std::clamp(m_audio.sfxVolume + delta, 0.f, 1.f);
        break;
    }
    case 8:
        break; // Back — handled below
    }

    const bool confirm = input.isKeyJustPressed(Key::Enter) || input.isKeyJustPressed(Key::Space) ||
                         input.isMouseButtonJustPressed(MouseButton::Left) ||
                         input.isGamepadButtonJustPressed(0, GamepadButton::A);
    const bool back = input.isKeyJustPressed(Key::Escape) || input.isGamepadButtonJustPressed(0, GamepadButton::B);

    if ((confirm && m_focusedRow == 8) || back) {
        applyAndSave();
        return m_returnTarget;
    }

    return Screen::Settings;
}

std::span<const HudElement> SettingsScreen::buildElements() {
    m_elementCount = 0;
    int si = 0; // next free string slot

    // Background
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Rect;
        el.x = 0.f;
        el.y = 0.f;
        el.x2 = 1.f;
        el.y2 = 1.f;
        el.r = 0.f;
        el.g = 0.f;
        el.b = 0.f;
        el.a = 1.f;
    }

    // Title
    m_strings[static_cast<std::size_t>(si)] = "SETTINGS";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si++)];
        el.x = 0.5f;
        el.y = 0.10f;
        el.scale = 1.5f;
        el.r = 1.f;
        el.g = 1.f;
        el.b = 1.f;
        el.a = 1.f;
    }

    // Build a data row: label (left) + value (right)
    // Returns two elements; consumes two string slots from si
    auto row = [&](int rowIdx, float y, std::string label, std::string value) {
        const bool focused = (rowIdx == m_focusedRow);

        m_strings[static_cast<std::size_t>(si)] = std::move(label);
        {
            auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
            el = HudElement{};
            el.type = HudElement::Type::Text;
            el.text = m_strings[static_cast<std::size_t>(si++)];
            el.x = 0.33f;
            el.y = y;
            el.r = focused ? 1.0f : 0.7f;
            el.g = focused ? 1.0f : 0.7f;
            el.b = focused ? 1.0f : 0.7f;
            el.a = 1.f;
        }

        m_strings[static_cast<std::size_t>(si)] = std::move(value);
        {
            auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
            el = HudElement{};
            el.type = HudElement::Type::Text;
            el.text = m_strings[static_cast<std::size_t>(si++)];
            el.x = 0.67f;
            el.y = y;
            el.r = focused ? 0.2f : 0.5f;
            el.g = focused ? 1.0f : 0.8f;
            el.b = focused ? 0.2f : 0.5f;
            el.a = 1.f;
        }
    };

    // Resolution
    std::string resVal = m_modes.empty() ? "Native"
                                         : std::to_string(m_modes[static_cast<std::size_t>(m_modeIdx)].first) + "x" +
                                               std::to_string(m_modes[static_cast<std::size_t>(m_modeIdx)].second);
    row(0, 0.20f, "Resolution:", resVal);
    row(1, 0.27f, "Display:", m_fullscreen ? "Fullscreen" : "Windowed");
    row(2, 0.34f, "Vsync:", vsyncLabel(m_graphics.vsync));
    row(3, 0.41f, "Anti-aliasing:", m_graphics.antiAliasing ? "On" : "Off");
    row(4, 0.48f, "Draw distance:", drawDistLabel(m_graphics.drawDistance));

    auto volStr = [](float v) { return std::to_string(static_cast<int>(std::round(v * 100.f))) + "%"; };
    row(5, 0.58f, "Master volume:", volStr(m_audio.masterVolume));
    row(6, 0.65f, "Music volume:", volStr(m_audio.musicVolume));
    row(7, 0.72f, "SFX volume:", volStr(m_audio.sfxVolume));

    // Back button
    m_strings[static_cast<std::size_t>(si)] = "[ Back ]";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si++)];
        el.x = 0.5f;
        el.y = 0.82f;
        el.r = (m_focusedRow == 8) ? 0.2f : 0.7f;
        el.g = (m_focusedRow == 8) ? 1.0f : 0.7f;
        el.b = (m_focusedRow == 8) ? 0.2f : 0.7f;
        el.a = 1.f;
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}
