// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IInput.h"
#include "SDL3EventSink.h"
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Gamepad;
typedef uint32_t SDL_JoystickID;

class SDL3Input : public IInput, public ISDL3EventSink {
  public:
    // --- IInput ---
    bool isKeyDown(Key key) const override;
    bool isKeyJustPressed(Key key) const override;

    void getMousePosition(int& x, int& y) const override;
    void getMouseDelta(int& dx, int& dy) const override;
    void setMouseCapture(bool capture) override;
    int getMouseScroll() const override;
    bool isMouseButtonDown(MouseButton button) const override;

    void startTextInput(ITextInputHandler* handler) override;
    void stopTextInput() override;

    void flush() override;

    int getGamepadCount() const override;
    bool isGamepadButtonDown(int gamepadId, GamepadButton button) const override;
    bool isGamepadButtonJustPressed(int gamepadId, GamepadButton button) const override;
    float getGamepadAxis(int gamepadId, GamepadAxis axis) const override;
    void rumble(int gamepadId, float lowFreq, float highFreq, uint32_t durationMs) override;
    void rumbleTriggers(int gamepadId, float leftRumble, float rightRumble, uint32_t durationMs) override;
    bool supportsRumble(int gamepadId) const override;
    bool supportsTriggerRumble(int gamepadId) const override;
    void stopRumble(int gamepadId) override;

    // --- ISDL3EventSink ---
    void onSDLEvent(const SDL_Event& ev) override;

  private:
    static constexpr int kKeyCount = static_cast<int>(Key::Count);
    static constexpr int kButtonCount = static_cast<int>(GamepadButton::Count);
    static constexpr int kAxisCount = static_cast<int>(GamepadAxis::Count);
    static constexpr int kMouseCount = static_cast<int>(MouseButton::Count);

    bool m_keys[kKeyCount]{};
    bool m_keysJustPressed[kKeyCount]{};

    int m_mouseX{0};
    int m_mouseY{0};
    int m_mouseDx{0};
    int m_mouseDy{0};
    int m_mouseScroll{0};
    bool m_mouseButtons[kMouseCount]{};
    bool m_mouseCaptured{false};

    struct GamepadState {
        SDL_JoystickID sdlId{0};
        SDL_Gamepad* handle{nullptr};
        bool buttons[kButtonCount]{};
        bool justPressed[kButtonCount]{};
        float axes[kAxisCount]{};
    };
    std::vector<GamepadState> m_gamepads;

    ITextInputHandler* m_textHandler{nullptr};

    // Returns the index into m_gamepads for the given SDL instance ID, or -1.
    int findGamepad(SDL_JoystickID id) const;
    // Returns a GamepadState pointer for a public gamepadId index, or nullptr.
    GamepadState* gamepadAt(int gamepadId);
    const GamepadState* gamepadAt(int gamepadId) const;
};
