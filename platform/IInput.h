// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// Engine-owned key codes. No SDL scancode values may appear here.
enum class Key : uint32_t {
    Unknown = 0,

    // Letters
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    // Digits (top row)
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    // Navigation
    Space,
    Enter,
    Tab,
    Backspace,
    Delete,
    Escape,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,

    // Function keys
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    // Modifiers
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,

    Count
};

enum class MouseButton : uint8_t { Left = 0, Middle, Right, Count };

enum class GamepadButton : uint8_t {
    A = 0,
    B,
    X,
    Y,
    LeftShoulder,
    RightShoulder,
    LeftTrigger,
    RightTrigger, // digital (pressed / not pressed)
    LeftStick,
    RightStick,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Start,
    Back,
    Count
};

// Analog axes; getGamepadAxis returns values in [-1.0, 1.0].
enum class GamepadAxis : uint8_t { LeftX = 0, LeftY, RightX, RightY, TriggerLeft, TriggerRight, Count };

// Register with IInput::startTextInput to receive text events from the OS IME
// pipeline. This is separate from isKeyDown — use it for any UI that accepts
// free-form text (player name, chat, mission editor fields).
// Threading: callbacks are invoked from the main thread inside IWindow::pollEvents().
class ITextInputHandler {
  public:
    virtual ~ITextInputHandler() = default;

    // Called with each committed UTF-8 string (may be more than one character).
    virtual void onTextInput(const char* text) = 0;

    // Called during IME composition with the in-progress string and cursor position.
    // composition may be empty when composition is cancelled.
    virtual void onTextEdit(const char* composition, int cursorPos) = 0;
};

// Threading: all methods must be called from the main thread.
class IInput {
  public:
    virtual ~IInput() = default;

    // --- Keyboard ---

    // True while the key is held down.
    virtual bool isKeyDown(Key key) const = 0;

    // True only on the first frame the key is pressed (one-frame pulse).
    virtual bool isKeyJustPressed(Key key) const = 0;

    // --- Mouse ---

    virtual void getMousePosition(int& x, int& y) const = 0;

    // Returns relative movement since the last frame; used for mouse-look in flight.
    virtual void getMouseDelta(int& dx, int& dy) const = 0;

    // Captures the mouse cursor for flight view; releases for menus.
    virtual void setMouseCapture(bool capture) = 0;

    // Positive = scroll up, negative = scroll down.
    virtual int getMouseScroll() const = 0;

    virtual bool isMouseButtonDown(MouseButton button) const = 0;

    // --- Text input ---

    // Activates OS text input mode (shows on-screen keyboard on mobile, enables IME
    // on desktop). Events are delivered to handler until stopTextInput is called.
    virtual void startTextInput(ITextInputHandler* handler) = 0;
    virtual void stopTextInput() = 0;

    // --- Frame boundary ---

    // Must be called once per frame by the game loop, after all input has been
    // read for that frame. Resets isKeyJustPressed for all keys, clears mouse
    // delta (dx, dy), and zeroes the scroll accumulator.
    virtual void flush() = 0;

    // --- Gamepad ---

    virtual int getGamepadCount() const = 0;
    virtual bool isGamepadButtonDown(int gamepadId, GamepadButton button) const = 0;
    virtual bool isGamepadButtonJustPressed(int gamepadId, GamepadButton button) const = 0;
    virtual float getGamepadAxis(int gamepadId, GamepadAxis axis) const = 0;

    // Values 0.0–1.0; backend scales to hardware range. Always a no-op if
    // hardware is unsupported — check supportsRumble / supportsTriggerRumble
    // first if behaviour should be gated on capability.
    virtual void rumble(int gamepadId, float lowFreq, float highFreq, uint32_t durationMs) = 0;
    virtual void rumbleTriggers(int gamepadId, float leftRumble, float rightRumble, uint32_t durationMs) = 0;

    // Returns true if the controller at gamepadId has main-motor rumble hardware.
    // Always false for out-of-range ids.
    virtual bool supportsRumble(int gamepadId) const = 0;

    // Returns true if the controller at gamepadId has trigger-motor rumble hardware.
    // Always false for out-of-range ids.
    virtual bool supportsTriggerRumble(int gamepadId) const = 0;

    // Stops all in-progress rumble (main motors + triggers) immediately.
    // No-op for out-of-range ids.
    virtual void stopRumble(int gamepadId) = 0;
};
