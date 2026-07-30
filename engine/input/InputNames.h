// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Binding.h"
#include <optional>
#include <string>

// The enum <-> name tables shared by everything that reads or writes bindings.toml.
//
// ONE table per enum, scanned in both directions (the #1050 rule): two hand-written switch statements
// per enum are two chances to disagree, and a single table cannot drift from itself. They live in
// their own header because bindings and axis configs BOTH name a GamepadAxis — when the axis-config
// table was re-keyed by (device, axis) in #1061 it needed the same names InputBindings serializes, and
// a second copy of that list would have been the same defect one level down.
//
// Every lookup returns nullopt / nullptr rather than a sentinel enumerator, so a caller cannot
// mistake "not a name I know" for a legitimate value.

namespace fl {

[[nodiscard]] const char* bindingSourceName(BindingSource s);
[[nodiscard]] std::optional<BindingSource> bindingSourceFromName(const std::string& name);

[[nodiscard]] const char* keyName(Key k);
[[nodiscard]] std::optional<Key> keyFromName(const std::string& name);

[[nodiscard]] const char* mouseButtonName(MouseButton b);
[[nodiscard]] std::optional<MouseButton> mouseButtonFromName(const std::string& name);

[[nodiscard]] const char* gamepadButtonName(GamepadButton b);
[[nodiscard]] std::optional<GamepadButton> gamepadButtonFromName(const std::string& name);

[[nodiscard]] const char* gamepadAxisName(GamepadAxis a);
[[nodiscard]] std::optional<GamepadAxis> gamepadAxisFromName(const std::string& name);

// Hat directions. Centered is nameable ("Centered") so a round-trip is total, but it is never a valid
// BINDING direction — a centered hat is a hat nobody is pressing.
[[nodiscard]] const char* hatPositionName(HatPosition p);
[[nodiscard]] std::optional<HatPosition> hatPositionFromName(const std::string& name);

} // namespace fl
