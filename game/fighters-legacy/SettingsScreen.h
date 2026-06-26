// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include "config/AudioSettings.h"
#include "config/GraphicsSettings.h"

#include <array>
#include <string>
#include <vector>

namespace fl {

class IDisplay;
class IRenderer;
class IWindow;
class UserConfig;

// Settings screen: graphics (resolution, display, vsync, AA mode, shadow quality,
// particle density, draw distance) and audio (master/music/SFX volumes).
// Copies UserConfig on entry; saves and applies on Back/Escape.
class SettingsScreen : public IScreen {
  public:
    SettingsScreen(UserConfig& config, IRenderer& renderer, IWindow& window, IDisplay& display);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

    // Where to return when the user presses Back.
    void setReturnTarget(Screen target) {
        m_returnTarget = target;
    }

  private:
    UserConfig& m_userConfig;
    IRenderer& m_renderer;
    IWindow& m_window;
    IDisplay& m_display;

    GraphicsSettings m_graphics;
    AudioSettings m_audio;
    Screen m_returnTarget{Screen::MainMenu};

    // Cached display modes for the current monitor
    std::vector<std::pair<int, int>> m_modes; // (width, height) unique entries
    int m_modeIdx{0};                         // current selection in m_modes
    bool m_fullscreen{false};

    // 0=Resolution, 1=Display, 2=Vsync, 3=AAMode, 4=ShadowQuality, 5=AmbientOcclusion,
    // 6=SkyQuality, 7=ParticleDensity, 8=DrawDist, 9=MasterVol, 10=MusicVol, 11=SfxVol, 12=Back
    int m_focusedRow{0};
    static constexpr int kRowCount = 13;

    void applyAndSave();
    void buildModes();

    static constexpr int kMaxElements = 32;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{};
    int m_elementCount{0};

    void buildRow(int row, float y, const std::string& label, const std::string& value);
};

} // namespace fl
