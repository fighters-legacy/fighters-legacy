// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

class IInput;
class IJoystick;
class JoystickDevices;

// The live input hardware a binding can resolve against.
//
// One struct rather than a growing parameter list. `bindingDown(in, b, gamepadId)` was already three
// arguments before #1061 added a second HAL and a device table; threading four or five through
// FlightInputCollector, CameraInput, FlightScreen and Game.cpp would put the same call-site edit in
// front of every future device class. This is the seam a head tracker or a second gamepad extends.
//
// EVERY POINTER MAY BE NULL, and null is normative: it means "that hardware is not present here", so
// bindings on it are inert rather than an error — the same rule a null renderer follows elsewhere in
// the engine. A headless build, a unit test with only a MockInput, and a machine with no stick all
// take the same path.
//
// `devices` null with a live `joystick` still works: only `Any`-device bindings resolve, which is the
// pre-#1061 behaviour of always reading device 0.
struct InputSources {
    IInput* input{nullptr};
    IJoystick* joystick{nullptr};
    const JoystickDevices* devices{nullptr};
    // Which gamepad IInput should be asked about. Gamepads are addressed by index, not GUID: SDL's
    // gamepad layer already presents them as an interchangeable standard layout, so "the pad" is a
    // meaningful thing to bind in a way "the stick" is not.
    int gamepadId{0};
};

} // namespace fl
