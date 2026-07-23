// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// 8-direction hat position plus centered.
enum class HatPosition : uint8_t {
    Centered = 0,
    Up,
    UpRight,
    Right,
    DownRight,
    Down,
    DownLeft,
    Left,
    UpLeft,
};

// Callback target for joystick hot-plug events.
// Threading: callbacks are invoked from the main thread inside IWindow::pollEvents().
class IJoystickEventHandler {
  public:
    virtual ~IJoystickEventHandler() = default;

    // joystickId is the new sequential index assigned by IJoystick.
    virtual void onJoystickAdded(int joystickId) = 0;

    // joystickId is the index that is about to be removed.
    virtual void onJoystickRemoved(int joystickId) = 0;
};

// Raw joystick / HOTAS interface. Covers devices that use SDL3's SDL_Joystick API —
// throttle quadrants, rudder pedals, hat switches, and joysticks with arbitrary axis
// counts — which cannot be represented by the fixed GamepadAxis / GamepadButton enums
// in IInput. IInput (SDL_Gamepad) and IJoystick (SDL_Joystick) are peers; a device
// recognised as a standard gamepad is owned exclusively by IInput.
//
// Threading: all methods must be called from the main thread.
class IJoystick {
  public:
    virtual ~IJoystick() = default;

    // --- Device enumeration ---

    virtual int getJoystickCount() const = 0;

    // Human-readable device name; stable for the lifetime of the connection.
    virtual const char* getJoystickName(int joystickId) const = 0;

    // GUID string (SDL GUID text form, 32 hex characters); use for persistent binding saves
    // so bindings survive device reconnect.
    virtual const char* getJoystickGuid(int joystickId) const = 0;

    // --- Axes (arbitrary count per device) ---

    virtual int getAxisCount(int joystickId) const = 0;

    // Returns a value in [-1.0, 1.0].
    virtual float getAxisValue(int joystickId, int axisIndex) const = 0;

    // --- Hat switches ---

    virtual int getHatCount(int joystickId) const = 0;
    virtual HatPosition getHatPosition(int joystickId, int hatIndex) const = 0;

    // --- Buttons (arbitrary count per device) ---

    virtual int getButtonCount(int joystickId) const = 0;
    virtual bool isButtonDown(int joystickId, int buttonIndex) const = 0;

    // True only on the first frame the button is pressed (one-frame pulse).
    virtual bool isButtonJustPressed(int joystickId, int buttonIndex) const = 0;

    // --- Hot-plug ---

    // Register a handler to receive add/remove notifications. Pass nullptr to clear.
    virtual void setEventHandler(IJoystickEventHandler* handler) = 0;

    // --- Frame boundary ---

    // Clears isButtonJustPressed for all devices. Must be called once per frame after
    // all input has been read, alongside IInput::flush().
    virtual void flush() = 0;

    virtual const char* getLastError() const = 0;

    // --- Force feedback (FFB sticks, #928) ---
    // A cueing layer, NOT a control-loading sim: these are canned haptic effects (stall buffet, ground
    // rumble, gun-fire kick), not modelled stick forces. The methods are NON-pure with capability-
    // negative no-op defaults, so gamepad-only backends and test mocks need no changes.
    static constexpr int kFfbSlotCount = 4;

    // Effect kinds: a steady push (ConstantForce) or an oscillation (Sine, for buffet / rumble).
    enum class FfbEffectKind : uint8_t { ConstantForce, Sine };

    struct FfbEffect {
        FfbEffectKind kind{FfbEffectKind::Sine};
        float directionDeg{0.0f}; // polar direction (SDL polar); 0 = away from the user
        float magnitude{0.0f};    // [0, 1]
        float periodMs{50.0f};    // Sine only
        uint32_t durationMs{0};   // 0 = run until stopped / updated
    };

    virtual bool supportsForceFeedback(int /*joystickId*/) const {
        return false;
    }
    // Re-playing a slot with the same kind updates the running effect in place (e.g. buffet amplitude
    // tracking); a different kind replaces it. Returns false when unsupported. slot in [0, kFfbSlotCount).
    virtual bool playFfbEffect(int /*joystickId*/, int /*slot*/, const FfbEffect& /*effect*/) {
        return false;
    }
    virtual void stopFfbEffect(int /*joystickId*/, int /*slot*/) {}
    virtual void stopAllFfbEffects(int /*joystickId*/) {}
};

} // namespace fl
