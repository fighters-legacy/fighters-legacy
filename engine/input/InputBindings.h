// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Binding.h"
#include "InputAction.h"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// Where a binding sits for one action. Three slots, not two (#1050): the gun trigger is Space AND
// the left mouse button AND a gamepad shoulder, and with only "primary + alt" one of those three had
// to be hardcoded somewhere in the game layer — which is precisely how a control ends up outside the
// binding table and outside the conflict check.
enum class BindingSlot : uint8_t {
    Primary = 0, // the keyboard default
    Secondary,   // a second keyboard/mouse binding for the same action
    Gamepad,     // the pad / stick binding
    Count
};

// Manages the full table of input bindings (three slots per action).
//
// THIS TABLE IS THE AUTHORITY (#1050). Every gameplay control the game reads resolves through it —
// a raw `isKeyDown(Key::V)` in the game layer is a bug, because it makes the control unrebindable
// and invisible to the conflict check. That is not hypothetical: master arm was a hardcoded `V`
// while the radio push-to-talk default was also `V`, so keying the radio safed the guns and
// rebinding either action fixed nothing.
//
// Serialization: serialize() returns a TOML string; deserialize() parses one.
// File I/O is the caller's responsibility — pass the result of IFilesystem::readFile
// to deserialize() and write serialize()'s output back via IFilesystem::writeFile.
// The intended path in the user-data domain is "config/bindings.toml".
class InputBindings {
  public:
    static constexpr int kActionCount = static_cast<int>(InputAction::Count);
    static constexpr int kSlotCount = static_cast<int>(BindingSlot::Count);

    // Bumped when the shipped key map changes in a way a stored file would otherwise undo. A saved
    // bindings.toml is a full table, not a patch: without a version, an install carrying the old
    // file would load the OLD defaults back over the new ones and quietly restore the very
    // collisions #1050 removed — for every player who never customised anything. Version 1 = the
    // pre-#1050 `[primary]`/`[alt]` map; version 2 = the three-slot, conflict-free map.
    static constexpr int kFormatVersion = 2;

    // The `version` value of a stored file, or 0 when it predates versioning. Callers regenerate
    // the file when this is below kFormatVersion.
    [[nodiscard]] static int fileFormatVersion(const std::string& toml);

    // One pair of actions that would both fire from the same physical input in the same session
    // mode. Reported by findConflicts(), which is what holds applyDefaults() to the same rule a
    // user rebind has always been held to.
    struct Conflict {
        InputAction a{};
        BindingSlot slotA{};
        InputAction b{};
        BindingSlot slotB{};
        Binding binding{};
    };

    InputBindings();

    void applyDefaults();

    [[nodiscard]] Binding get(InputAction action, BindingSlot slot = BindingSlot::Primary) const;
    void set(InputAction action, Binding binding, BindingSlot slot = BindingSlot::Primary);
    void clear(InputAction action, BindingSlot slot = BindingSlot::Primary);

    // The session modes in which the game reads this action. Two actions can share a binding
    // without conflicting when their context sets are disjoint — see InputContext.
    [[nodiscard]] static InputContext contexts(InputAction action) noexcept;

    [[nodiscard]] static const char* actionName(InputAction action);
    [[nodiscard]] static std::optional<InputAction> actionFromName(const std::string& name);
    [[nodiscard]] static const char* slotName(BindingSlot slot);

    // Returns the first action that already uses the given binding IN AN OVERLAPPING CONTEXT
    // (excluding 'skipAction' so you can re-assign an action's own binding without a false
    // positive). Returns nullopt if no conflict.
    [[nodiscard]] std::optional<InputAction> conflictsWith(InputAction skipAction, const Binding& binding) const;

    // Every conflicting pair in the whole table, lowest action ordinal first. Empty = the table is
    // self-consistent. Used by the defaults test and by the game at startup to warn about a
    // hand-edited bindings.toml.
    [[nodiscard]] std::vector<Conflict> findConflicts() const;

    // Serializes all bindings to a TOML string.
    [[nodiscard]] std::string serialize() const;

    // Parses a TOML string into the binding table. On parse failure the existing
    // state is unchanged and false is returned.
    bool deserialize(const std::string& toml);

  private:
    std::array<std::array<Binding, kActionCount>, kSlotCount> m_slots{};

    static std::string serializeBinding(const Binding& b);
    static bool parseBinding(const std::string& source, const std::string& id, bool axisNegative, Binding& out);
};

} // namespace fl
