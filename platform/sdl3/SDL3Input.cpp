// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "SDL3Input.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace fl {

// ---------------------------------------------------------------------------
// Mapping helpers (SDL → engine enums)
// ---------------------------------------------------------------------------

// The keyboard map, in one place (#1265).
//
// This was two inverse 96-case switches. They were a perfect bijection when checked -- no duplicate
// scancode, no duplicate Key, same order in both -- but nothing made them stay one: adding a key
// meant editing two switches, and forgetting the second half produces a key that the game reads and
// can never rebind, or one that rebinds to nothing. Neither failure is visible until a player finds
// it, because there is no SDL3 test in the suite to catch it.
//
// One table, read in both directions, makes the inverse structural. A linear scan over 96 entries is
// nothing next to the SDL event dispatch that precedes it on the read path, and the write path is
// the key-name lookup, which is cold.
constexpr std::pair<SDL_Scancode, Key> kKeyMap[] = {
    {SDL_SCANCODE_A, Key::A},
    {SDL_SCANCODE_B, Key::B},
    {SDL_SCANCODE_C, Key::C},
    {SDL_SCANCODE_D, Key::D},
    {SDL_SCANCODE_E, Key::E},
    {SDL_SCANCODE_F, Key::F},
    {SDL_SCANCODE_G, Key::G},
    {SDL_SCANCODE_H, Key::H},
    {SDL_SCANCODE_I, Key::I},
    {SDL_SCANCODE_J, Key::J},
    {SDL_SCANCODE_K, Key::K},
    {SDL_SCANCODE_L, Key::L},
    {SDL_SCANCODE_M, Key::M},
    {SDL_SCANCODE_N, Key::N},
    {SDL_SCANCODE_O, Key::O},
    {SDL_SCANCODE_P, Key::P},
    {SDL_SCANCODE_Q, Key::Q},
    {SDL_SCANCODE_R, Key::R},
    {SDL_SCANCODE_S, Key::S},
    {SDL_SCANCODE_T, Key::T},
    {SDL_SCANCODE_U, Key::U},
    {SDL_SCANCODE_V, Key::V},
    {SDL_SCANCODE_W, Key::W},
    {SDL_SCANCODE_X, Key::X},
    {SDL_SCANCODE_Y, Key::Y},
    {SDL_SCANCODE_Z, Key::Z},
    {SDL_SCANCODE_0, Key::Num0},
    {SDL_SCANCODE_1, Key::Num1},
    {SDL_SCANCODE_2, Key::Num2},
    {SDL_SCANCODE_3, Key::Num3},
    {SDL_SCANCODE_4, Key::Num4},
    {SDL_SCANCODE_5, Key::Num5},
    {SDL_SCANCODE_6, Key::Num6},
    {SDL_SCANCODE_7, Key::Num7},
    {SDL_SCANCODE_8, Key::Num8},
    {SDL_SCANCODE_9, Key::Num9},
    {SDL_SCANCODE_SPACE, Key::Space},
    {SDL_SCANCODE_RETURN, Key::Enter},
    {SDL_SCANCODE_TAB, Key::Tab},
    {SDL_SCANCODE_BACKSPACE, Key::Backspace},
    {SDL_SCANCODE_DELETE, Key::Delete},
    {SDL_SCANCODE_ESCAPE, Key::Escape},
    {SDL_SCANCODE_UP, Key::ArrowUp},
    {SDL_SCANCODE_DOWN, Key::ArrowDown},
    {SDL_SCANCODE_LEFT, Key::ArrowLeft},
    {SDL_SCANCODE_RIGHT, Key::ArrowRight},
    {SDL_SCANCODE_HOME, Key::Home},
    {SDL_SCANCODE_END, Key::End},
    {SDL_SCANCODE_PAGEUP, Key::PageUp},
    {SDL_SCANCODE_PAGEDOWN, Key::PageDown},
    {SDL_SCANCODE_INSERT, Key::Insert},
    {SDL_SCANCODE_F1, Key::F1},
    {SDL_SCANCODE_F2, Key::F2},
    {SDL_SCANCODE_F3, Key::F3},
    {SDL_SCANCODE_F4, Key::F4},
    {SDL_SCANCODE_F5, Key::F5},
    {SDL_SCANCODE_F6, Key::F6},
    {SDL_SCANCODE_F7, Key::F7},
    {SDL_SCANCODE_F8, Key::F8},
    {SDL_SCANCODE_F9, Key::F9},
    {SDL_SCANCODE_F10, Key::F10},
    {SDL_SCANCODE_F11, Key::F11},
    {SDL_SCANCODE_F12, Key::F12},
    {SDL_SCANCODE_LSHIFT, Key::LeftShift},
    {SDL_SCANCODE_RSHIFT, Key::RightShift},
    {SDL_SCANCODE_LCTRL, Key::LeftCtrl},
    {SDL_SCANCODE_RCTRL, Key::RightCtrl},
    {SDL_SCANCODE_LALT, Key::LeftAlt},
    {SDL_SCANCODE_RALT, Key::RightAlt},
    {SDL_SCANCODE_MINUS, Key::Minus},
    {SDL_SCANCODE_EQUALS, Key::Equals},
    {SDL_SCANCODE_COMMA, Key::Comma},
    {SDL_SCANCODE_PERIOD, Key::Period},
    {SDL_SCANCODE_SLASH, Key::Slash},
    {SDL_SCANCODE_SEMICOLON, Key::Semicolon},
    {SDL_SCANCODE_APOSTROPHE, Key::Apostrophe},
    {SDL_SCANCODE_LEFTBRACKET, Key::LeftBracket},
    {SDL_SCANCODE_RIGHTBRACKET, Key::RightBracket},
    {SDL_SCANCODE_BACKSLASH, Key::Backslash},
    {SDL_SCANCODE_GRAVE, Key::Grave},
    {SDL_SCANCODE_KP_0, Key::Numpad0},
    {SDL_SCANCODE_KP_1, Key::Numpad1},
    {SDL_SCANCODE_KP_2, Key::Numpad2},
    {SDL_SCANCODE_KP_3, Key::Numpad3},
    {SDL_SCANCODE_KP_4, Key::Numpad4},
    {SDL_SCANCODE_KP_5, Key::Numpad5},
    {SDL_SCANCODE_KP_6, Key::Numpad6},
    {SDL_SCANCODE_KP_7, Key::Numpad7},
    {SDL_SCANCODE_KP_8, Key::Numpad8},
    {SDL_SCANCODE_KP_9, Key::Numpad9},
    {SDL_SCANCODE_KP_PLUS, Key::NumpadPlus},
    {SDL_SCANCODE_KP_MINUS, Key::NumpadMinus},
    {SDL_SCANCODE_KP_MULTIPLY, Key::NumpadMultiply},
    {SDL_SCANCODE_KP_DIVIDE, Key::NumpadDivide},
    {SDL_SCANCODE_KP_PERIOD, Key::NumpadPeriod},
    {SDL_SCANCODE_KP_ENTER, Key::NumpadEnter},
};

// The bijection, checked by the compiler rather than by whoever last added a row: a duplicate on
// either side would silently make one direction pick the first match and the other the second.
constexpr bool keyMapIsBijective() {
    for (std::size_t i = 0; i < std::size(kKeyMap); ++i)
        for (std::size_t j = i + 1; j < std::size(kKeyMap); ++j)
            if (kKeyMap[i].first == kKeyMap[j].first || kKeyMap[i].second == kKeyMap[j].second)
                return false;
    return true;
}
static_assert(keyMapIsBijective(), "kKeyMap must map each scancode and each Key exactly once");

// An unmapped input is Unknown in either direction -- the `default:` arms the switches carried.
static Key fromSDLScancode(SDL_Scancode sc) {
    for (const auto& [scancode, key] : kKeyMap)
        if (scancode == sc)
            return key;
    return Key::Unknown;
}

static SDL_Scancode toSDLScancode(Key k) {
    for (const auto& [scancode, key] : kKeyMap)
        if (key == k)
            return scancode;
    return SDL_SCANCODE_UNKNOWN;
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
        if (mb != MouseButton::Count) {
            m_mouseButtons[static_cast<int>(mb)] = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                m_mouseJustPressed[static_cast<int>(mb)] = true;
        }
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

const char* SDL3Input::getKeyName(Key key) const {
    if (key == Key::Unknown || key >= Key::Count)
        return "Unknown";
    SDL_Scancode sc = toSDLScancode(key);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return "Unknown";
    SDL_Keycode kc = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    const char* name = SDL_GetKeyName(kc);
    return (name && name[0] != '\0') ? name : "Unknown";
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

bool SDL3Input::isMouseButtonJustPressed(MouseButton button) const {
    if (button >= MouseButton::Count)
        return false;
    return m_mouseJustPressed[static_cast<int>(button)];
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
    for (int i = 0; i < kMouseCount; ++i)
        m_mouseJustPressed[i] = false;
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

} // namespace fl
