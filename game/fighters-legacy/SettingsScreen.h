// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include "config/AudioSettings.h"
#include "config/GraphicsSettings.h"
#include "config/VoiceSettings.h"

#include <array>
#include <string>
#include <vector>

namespace fl {

class IDisplay;
class IRenderer;
class IWindow;
class UserConfig;

// Settings screen: graphics (resolution, display, vsync, AA mode, shadow quality,
// particle density, draw distance), audio (master/music/SFX/voice volumes) and voice comms
// (Epic J: enable, keying mode, input device, mic gain, radio effect, ducking).
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

    // Capture devices to offer on the input-device row. Filled by Game.cpp from IAudioCapture; an
    // empty list simply leaves the row on "Default" rather than hiding it, so a player with no
    // device still sees WHY they cannot talk.
    void setVoiceDevices(std::vector<std::string> devices);

  private:
    UserConfig& m_userConfig;
    IRenderer& m_renderer;
    IWindow& m_window;
    IDisplay& m_display;

    GraphicsSettings m_graphics;
    AudioSettings m_audio;
    VoiceSettings m_voice;
    Screen m_returnTarget{Screen::MainMenu};

    // Capture-device choices; index 0 is always "Default" (the empty device name).
    std::vector<std::string> m_voiceDevices{std::string{}};
    int m_voiceDeviceIdx{0};

    // Cached display modes for the current monitor
    std::vector<std::pair<int, int>> m_modes; // (width, height) unique entries
    int m_modeIdx{0};                         // current selection in m_modes
    bool m_fullscreen{false};

    // 0=Resolution, 1=Display, 2=Vsync, 3=AAMode, 4=ShadowQuality, 5=AmbientOcclusion,
    // 6=SkyQuality, 7=ParticleDensity, 8=DrawDist, 9=MasterVol, 10=MusicVol, 11=SfxVol,
    // 12=VoiceVol, 13=VoiceEnabled, 14=VoiceMode, 15=VoiceDevice, 16=MicGain, 17=RadioEffect,
    // 18=Ducking, 19=Back
    int m_focusedRow{0};
    static constexpr int kRowVoiceVolume = 12;
    static constexpr int kRowBack = 19;
    static constexpr int kRowCount = kRowBack + 1;

    // Row y is COMPUTED, not tabulated: the old hand-written table had to be re-derived by hand
    // every time a row was added, and the hover bands silently drifted off the drawn rows when it
    // was not. One formula keeps the hit-test and the render in lockstep by construction.
    static constexpr float kFirstRowY = 0.16f;
    static constexpr float kRowStep = 0.042f;
    static constexpr float kRowHitH = kRowStep;
    static constexpr float rowY(int rowIdx) {
        return kFirstRowY + kRowStep * static_cast<float>(rowIdx);
    }

    void applyAndSave();
    void buildModes();

    static constexpr int kMaxElements = 64;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{};
    int m_elementCount{0};
};

} // namespace fl
