// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IInput.h"
#include <cstdint>

namespace fl {

enum class BindingSource : uint8_t {
    None,
    Keyboard,
    MouseButton,
    GamepadButton,
    GamepadAxis,
};

struct Binding {
    BindingSource source{BindingSource::None};
    uint32_t id{0};           // Key / MouseButton / GamepadButton / GamepadAxis cast to uint32_t
    bool axisNegative{false}; // true = negative axis direction triggers a digital action

    bool isNone() const {
        return source == BindingSource::None;
    }
};

} // namespace fl
