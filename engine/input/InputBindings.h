// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Binding.h"
#include "InputAction.h"
#include <array>
#include <optional>
#include <string>

namespace fl {

// Manages the full table of input bindings (primary + alternate per action).
//
// Serialization: serialize() returns a TOML string; deserialize() parses one.
// File I/O is the caller's responsibility — pass the result of IFilesystem::readFile
// to deserialize() and write serialize()'s output back via IFilesystem::writeFile.
// The intended path in the user-data domain is "config/bindings.toml".
class InputBindings {
  public:
    static constexpr int kActionCount = static_cast<int>(InputAction::Count);

    InputBindings();

    void applyDefaults();

    Binding get(InputAction action, bool alt = false) const;
    void set(InputAction action, Binding binding, bool alt = false);
    void clear(InputAction action, bool alt = false);

    // Returns the first action that already uses the given binding (excluding
    // 'skipAction' so you can re-assign an action's own binding without a
    // false positive). Returns nullopt if no conflict.
    std::optional<InputAction> conflictsWith(InputAction skipAction, const Binding& binding) const;

    // Serializes all bindings to a TOML string.
    std::string serialize() const;

    // Parses a TOML string into the binding table. On parse failure the existing
    // state is unchanged and false is returned.
    bool deserialize(const std::string& toml);

  private:
    std::array<Binding, kActionCount> m_primary;
    std::array<Binding, kActionCount> m_alt;

    static const char* actionName(InputAction action);
    static std::optional<InputAction> actionFromName(const std::string& name);
    static std::string serializeBinding(const Binding& b);
    static bool parseBinding(const std::string& source, const std::string& id, bool axisNegative, Binding& out);
};

} // namespace fl
