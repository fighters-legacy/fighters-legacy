// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "SDL3Input.h"
#include <SDL3/SDL.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// Mapping helpers (SDL → engine enums)
// ---------------------------------------------------------------------------

static Key fromSDLScancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_A:
        return Key::A;
    case SDL_SCANCODE_B:
        return Key::B;
    case SDL_SCANCODE_C:
        return Key::C;
    case SDL_SCANCODE_D:
        return Key::D;
    case SDL_SCANCODE_E:
        return Key::E;
    case SDL_SCANCODE_F:
        return Key::F;
    case SDL_SCANCODE_G:
        return Key::G;
    case SDL_SCANCODE_H:
        return Key::H;
    case SDL_SCANCODE_I:
        return Key::I;
    case SDL_SCANCODE_J:
        return Key::J;
    case SDL_SCANCODE_K:
        return Key::K;
    case SDL_SCANCODE_L:
        return Key::L;
    case SDL_SCANCODE_M:
        return Key::M;
    case SDL_SCANCODE_N:
        return Key::N;
    case SDL_SCANCODE_O:
        return Key::O;
    case SDL_SCANCODE_P:
        return Key::P;
    case SDL_SCANCODE_Q:
        return Key::Q;
    case SDL_SCANCODE_R:
        return Key::R;
    case SDL_SCANCODE_S:
        return Key::S;
    case SDL_SCANCODE_T:
        return Key::T;
    case SDL_SCANCODE_U:
        return Key::U;
    case SDL_SCANCODE_V:
        return Key::V;
    case SDL_SCANCODE_W:
        return Key::W;
    case SDL_SCANCODE_X:
        return Key::X;
    case SDL_SCANCODE_Y:
        return Key::Y;
    case SDL_SCANCODE_Z:
        return Key::Z;
    case SDL_SCANCODE_0:
        return Key::Num0;
    case SDL_SCANCODE_1:
        return Key::Num1;
    case SDL_SCANCODE_2:
        return Key::Num2;
    case SDL_SCANCODE_3:
        return Key::Num3;
    case SDL_SCANCODE_4:
        return Key::Num4;
    case SDL_SCANCODE_5:
        return Key::Num5;
    case SDL_SCANCODE_6:
        return Key::Num6;
    case SDL_SCANCODE_7:
        return Key::Num7;
    case SDL_SCANCODE_8:
        return Key::Num8;
    case SDL_SCANCODE_9:
        return Key::Num9;
    case SDL_SCANCODE_SPACE:
        return Key::Space;
    case SDL_SCANCODE_RETURN:
        return Key::Enter;
    case SDL_SCANCODE_TAB:
        return Key::Tab;
    case SDL_SCANCODE_BACKSPACE:
        return Key::Backspace;
    case SDL_SCANCODE_DELETE:
        return Key::Delete;
    case SDL_SCANCODE_ESCAPE:
        return Key::Escape;
    case SDL_SCANCODE_UP:
        return Key::ArrowUp;
    case SDL_SCANCODE_DOWN:
        return Key::ArrowDown;
    case SDL_SCANCODE_LEFT:
        return Key::ArrowLeft;
    case SDL_SCANCODE_RIGHT:
        return Key::ArrowRight;
    case SDL_SCANCODE_HOME:
        return Key::Home;
    case SDL_SCANCODE_END:
        return Key::End;
    case SDL_SCANCODE_PAGEUP:
        return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN:
        return Key::PageDown;
    case SDL_SCANCODE_INSERT:
        return Key::Insert;
    case SDL_SCANCODE_F1:
        return Key::F1;
    case SDL_SCANCODE_F2:
        return Key::F2;
    case SDL_SCANCODE_F3:
        return Key::F3;
    case SDL_SCANCODE_F4:
        return Key::F4;
    case SDL_SCANCODE_F5:
        return Key::F5;
    case SDL_SCANCODE_F6:
        return Key::F6;
    case SDL_SCANCODE_F7:
        return Key::F7;
    case SDL_SCANCODE_F8:
        return Key::F8;
    case SDL_SCANCODE_F9:
        return Key::F9;
    case SDL_SCANCODE_F10:
        return Key::F10;
    case SDL_SCANCODE_F11:
        return Key::F11;
    case SDL_SCANCODE_F12:
        return Key::F12;
    case SDL_SCANCODE_LSHIFT:
        return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT:
        return Key::RightShift;
    case SDL_SCANCODE_LCTRL:
        return Key::LeftCtrl;
    case SDL_SCANCODE_RCTRL:
        return Key::RightCtrl;
    case SDL_SCANCODE_LALT:
        return Key::LeftAlt;
    case SDL_SCANCODE_RALT:
        return Key::RightAlt;
    default:
        return Key::Unknown;
    }
}

static GamepadButton fromSDLButton(SDL_GamepadButton b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return GamepadButton::A;
    case SDL_GAMEPAD_BUTTON_EAST:
        return GamepadButton::B;
    case SDL_GAMEPAD_BUTTON_WEST:
        return GamepadButton::X;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return GamepadButton::Y;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return GamepadButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return GamepadButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return GamepadButton::LeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return GamepadButton::RightStick;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return GamepadButton::DpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return GamepadButton::DpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return GamepadButton::DpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return GamepadButton::DpadRight;
    case SDL_GAMEPAD_BUTTON_START:
        return GamepadButton::Start;
    case SDL_GAMEPAD_BUTTON_BACK:
        return GamepadButton::Back;
    default:
        return GamepadButton::Count; // unrecognised
    }
}

static GamepadAxis fromSDLAxis(SDL_GamepadAxis a) {
    switch (a) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        return GamepadAxis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY:
        return GamepadAxis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX:
        return GamepadAxis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY:
        return GamepadAxis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        return GamepadAxis::TriggerLeft;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return GamepadAxis::TriggerRight;
    default:
        return GamepadAxis::Count; // unrecognised
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

int SDL3Input::findGamepad(SDL_JoystickID id) const {
    for (int i = 0; i < static_cast<int>(m_gamepads.size()); ++i) {
        if (m_gamepads[i].sdlId == id)
            return i;
    }
    return -1;
}

SDL3Input::GamepadState* SDL3Input::gamepadAt(int gamepadId) {
    if (gamepadId < 0 || gamepadId >= static_cast<int>(m_gamepads.size()))
        return nullptr;
    return &m_gamepads[static_cast<size_t>(gamepadId)];
}

const SDL3Input::GamepadState* SDL3Input::gamepadAt(int gamepadId) const {
    if (gamepadId < 0 || gamepadId >= static_cast<int>(m_gamepads.size()))
        return nullptr;
    return &m_gamepads[static_cast<size_t>(gamepadId)];
}

// Synthesise digital press/release for trigger buttons from axis values.
// Triggers report [0.0, 1.0]; use 0.5 press / 0.25 release thresholds (hysteresis).
static constexpr float kTriggerPressThreshold = 0.5f;
static constexpr float kTriggerReleaseThreshold = 0.25f;

static void updateTriggerButton(bool* buttons, bool* justPressed, int btnIdx, float axisValue) {
    bool wasPressed = buttons[btnIdx];
    bool nowPressed = wasPressed ? (axisValue >= kTriggerReleaseThreshold) : (axisValue >= kTriggerPressThreshold);
    buttons[btnIdx] = nowPressed;
    if (nowPressed && !wasPressed)
        justPressed[btnIdx] = true;
}

// ---------------------------------------------------------------------------
// ISDL3EventSink
// ---------------------------------------------------------------------------

void SDL3Input::onSDLEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (ev.key.repeat)
            break;
        Key k = fromSDLScancode(ev.key.scancode);
        if (k != Key::Unknown) {
            m_keys[static_cast<int>(k)] = true;
            m_keysJustPressed[static_cast<int>(k)] = true;
        }
        break;
    }
    case SDL_EVENT_KEY_UP: {
        Key k = fromSDLScancode(ev.key.scancode);
        if (k != Key::Unknown)
            m_keys[static_cast<int>(k)] = false;
        break;
    }

    case SDL_EVENT_MOUSE_MOTION:
        m_mouseX = static_cast<int>(ev.motion.x);
        m_mouseY = static_cast<int>(ev.motion.y);
        m_mouseDx += static_cast<int>(ev.motion.xrel);
        m_mouseDy += static_cast<int>(ev.motion.yrel);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        MouseButton mb = MouseButton::Count;
        if (ev.button.button == SDL_BUTTON_LEFT)
            mb = MouseButton::Left;
        else if (ev.button.button == SDL_BUTTON_MIDDLE)
            mb = MouseButton::Middle;
        else if (ev.button.button == SDL_BUTTON_RIGHT)
            mb = MouseButton::Right;
        if (mb != MouseButton::Count)
            m_mouseButtons[static_cast<int>(mb)] = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
        m_mouseScroll += static_cast<int>(ev.wheel.y);
        break;

    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad* handle = SDL_OpenGamepad(ev.gdevice.which);
        if (handle) {
            GamepadState gs;
            gs.sdlId = ev.gdevice.which;
            gs.handle = handle;
            m_gamepads.push_back(gs);
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int idx = findGamepad(ev.gdevice.which);
        if (idx >= 0) {
            SDL_CloseGamepad(m_gamepads[static_cast<size_t>(idx)].handle);
            m_gamepads.erase(m_gamepads.begin() + idx);
        }
        break;
    }

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int idx = findGamepad(ev.gbutton.which);
        if (idx < 0)
            break;
        GamepadButton gb = fromSDLButton(static_cast<SDL_GamepadButton>(ev.gbutton.button));
        if (gb == GamepadButton::Count)
            break;
        bool pressed = (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        GamepadState& gp = m_gamepads[static_cast<size_t>(idx)];
        gp.buttons[static_cast<int>(gb)] = pressed;
        if (pressed)
            gp.justPressed[static_cast<int>(gb)] = true;
        break;
    }

    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int idx = findGamepad(ev.gaxis.which);
        if (idx < 0)
            break;
        GamepadAxis ga = fromSDLAxis(static_cast<SDL_GamepadAxis>(ev.gaxis.axis));
        if (ga == GamepadAxis::Count)
            break;
        // Sint16 range: [-32768, 32767]. Normalize to [-1.0, 1.0].
        float value = static_cast<float>(ev.gaxis.value) / 32767.0f;
        value = std::max(-1.0f, std::min(1.0f, value));
        GamepadState& gp = m_gamepads[static_cast<size_t>(idx)];
        gp.axes[static_cast<int>(ga)] = value;
        // Synthesise digital state for trigger buttons from axis values.
        // Trigger axes report [0, 32767] normalized to [0.0, 1.0].
        if (ga == GamepadAxis::TriggerLeft)
            updateTriggerButton(gp.buttons, gp.justPressed, static_cast<int>(GamepadButton::LeftTrigger), value);
        else if (ga == GamepadAxis::TriggerRight)
            updateTriggerButton(gp.buttons, gp.justPressed, static_cast<int>(GamepadButton::RightTrigger), value);
        break;
    }

    case SDL_EVENT_TEXT_INPUT:
        if (m_textHandler)
            m_textHandler->onTextInput(ev.text.text);
        break;

    case SDL_EVENT_TEXT_EDITING:
        if (m_textHandler)
            m_textHandler->onTextEdit(ev.edit.text, ev.edit.start);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// IInput — keyboard
// ---------------------------------------------------------------------------

bool SDL3Input::isKeyDown(Key key) const {
    if (key == Key::Unknown || key >= Key::Count)
        return false;
    return m_keys[static_cast<int>(key)];
}

bool SDL3Input::isKeyJustPressed(Key key) const {
    if (key == Key::Unknown || key >= Key::Count)
        return false;
    return m_keysJustPressed[static_cast<int>(key)];
}

// ---------------------------------------------------------------------------
// IInput — mouse
// ---------------------------------------------------------------------------

void SDL3Input::getMousePosition(int& x, int& y) const {
    x = m_mouseX;
    y = m_mouseY;
}

void SDL3Input::getMouseDelta(int& dx, int& dy) const {
    dx = m_mouseDx;
    dy = m_mouseDy;
}

void SDL3Input::setMouseCapture(bool capture) {
    if (capture == m_mouseCaptured)
        return;
    m_mouseCaptured = capture;
    SDL_SetWindowRelativeMouseMode(SDL_GetMouseFocus(), capture);
}

int SDL3Input::getMouseScroll() const {
    return m_mouseScroll;
}

bool SDL3Input::isMouseButtonDown(MouseButton button) const {
    if (button >= MouseButton::Count)
        return false;
    return m_mouseButtons[static_cast<int>(button)];
}

// ---------------------------------------------------------------------------
// IInput — text input
// ---------------------------------------------------------------------------

void SDL3Input::startTextInput(ITextInputHandler* handler) {
    m_textHandler = handler;
    SDL_StartTextInput(SDL_GetKeyboardFocus());
}

void SDL3Input::stopTextInput() {
    SDL_StopTextInput(SDL_GetKeyboardFocus());
    m_textHandler = nullptr;
}

// ---------------------------------------------------------------------------
// IInput — frame boundary
// ---------------------------------------------------------------------------

void SDL3Input::flush() {
    for (int i = 0; i < kKeyCount; ++i)
        m_keysJustPressed[i] = false;
    for (auto& gp : m_gamepads) {
        for (int i = 0; i < kButtonCount; ++i)
            gp.justPressed[i] = false;
    }
    m_mouseDx = 0;
    m_mouseDy = 0;
    m_mouseScroll = 0;
}

// ---------------------------------------------------------------------------
// IInput — gamepad
// ---------------------------------------------------------------------------

int SDL3Input::getGamepadCount() const {
    return static_cast<int>(m_gamepads.size());
}

bool SDL3Input::isGamepadButtonDown(int gamepadId, GamepadButton button) const {
    const GamepadState* gp = gamepadAt(gamepadId);
    if (!gp || button >= GamepadButton::Count)
        return false;
    return gp->buttons[static_cast<int>(button)];
}

bool SDL3Input::isGamepadButtonJustPressed(int gamepadId, GamepadButton button) const {
    const GamepadState* gp = gamepadAt(gamepadId);
    if (!gp || button >= GamepadButton::Count)
        return false;
    return gp->justPressed[static_cast<int>(button)];
}

float SDL3Input::getGamepadAxis(int gamepadId, GamepadAxis axis) const {
    const GamepadState* gp = gamepadAt(gamepadId);
    if (!gp || axis >= GamepadAxis::Count)
        return 0.0f;
    return gp->axes[static_cast<int>(axis)];
}

void SDL3Input::rumble(int gamepadId, float lowFreq, float highFreq, uint32_t durationMs) {
    GamepadState* gp = gamepadAt(gamepadId);
    if (!gp)
        return;
    auto lo = static_cast<uint16_t>(lowFreq * 65535.0f);
    auto hi = static_cast<uint16_t>(highFreq * 65535.0f);
    SDL_RumbleGamepad(gp->handle, lo, hi, durationMs);
}

void SDL3Input::rumbleTriggers(int gamepadId, float leftRumble, float rightRumble, uint32_t durationMs) {
    GamepadState* gp = gamepadAt(gamepadId);
    if (!gp)
        return;
    auto l = static_cast<uint16_t>(leftRumble * 65535.0f);
    auto r = static_cast<uint16_t>(rightRumble * 65535.0f);
    SDL_RumbleGamepadTriggers(gp->handle, l, r, durationMs);
}

bool SDL3Input::supportsRumble(int gamepadId) const {
    const GamepadState* gp = gamepadAt(gamepadId);
    if (!gp)
        return false;
    SDL_PropertiesID props = SDL_GetGamepadProperties(gp->handle);
    return SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

bool SDL3Input::supportsTriggerRumble(int gamepadId) const {
    const GamepadState* gp = gamepadAt(gamepadId);
    if (!gp)
        return false;
    SDL_PropertiesID props = SDL_GetGamepadProperties(gp->handle);
    return SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
}

void SDL3Input::stopRumble(int gamepadId) {
    GamepadState* gp = gamepadAt(gamepadId);
    if (!gp)
        return;
    SDL_RumbleGamepad(gp->handle, 0, 0, 0);
    SDL_RumbleGamepadTriggers(gp->handle, 0, 0, 0);
}
