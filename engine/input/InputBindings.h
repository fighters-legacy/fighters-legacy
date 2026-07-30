// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Binding.h"
#include "InputAction.h"
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fl {

// A device the binding table has seen, remembered by GUID with the name it reported.
//
// The name is persisted for one reason: a GUID is 32 hex characters, so a warning that only quoted it
// would be useless to the player it is addressed to. With the name stored, an absent stick produces
// "bindings.toml references 'Thrustmaster T.16000M' … not connected" instead of a hex blob, and a
// hand-edited file has something legible to key its `device = "…"` fields off.
struct KnownDevice {
    DeviceRef ref;
    std::string name;
};

// Manages the full table of input bindings: an ARBITRARY-LENGTH list per action (#1061).
//
// THIS TABLE IS THE AUTHORITY (#1050). Every gameplay control the game reads resolves through it —
// a raw `isKeyDown(Key::V)` in the game layer is a bug, because it makes the control unrebindable
// and invisible to the conflict check. That is not hypothetical: master arm was a hardcoded `V`
// while the radio push-to-talk default was also `V`, so keying the radio safed the guns and
// rebinding either action fixed nothing.
//
// #1061 removed the three fixed slots. `[primary]`/`[secondary]`/`[gamepad]` only ever existed
// BECAUSE there was no list — the slot names were a storage shape leaking into the file format, and
// three of them could not express "the gun is Space, left mouse, a shoulder button, joystick 1 button
// 5 and joystick 2 button 7". Bindings are now an ordered list per action, and a binding names the
// DEVICE it belongs to, so two sticks are addressed independently.
//
// ORDER IS MEANINGFUL for analog axes: actionAxis() takes the first ACTIVE axis in the list, so the
// list is the player's priority. It is irrelevant for digital bindings, where any one being down is
// enough.
//
// Serialization: serialize() returns a TOML string; deserialize() parses one.
// File I/O is the caller's responsibility — pass the result of IFilesystem::readFile
// to deserialize() and write serialize()'s output back via IFilesystem::writeFile.
// The intended path in the user-data domain is "config/bindings.toml".
class InputBindings {
  public:
    static constexpr int kActionCount = static_cast<int>(InputAction::Count);

    // Bumped when the shipped key map or the file schema changes in a way a stored file would
    // otherwise undo. A saved bindings.toml is a full table, not a patch: without a version, an
    // install carrying the old file would load the OLD defaults back over the new ones and quietly
    // restore the very collisions #1050 removed — for every player who never customised anything.
    // Version 1 = the pre-#1050 `[primary]`/`[alt]` map; version 2 = the three-slot, conflict-free
    // map; version 3 = per-action binding LISTS with device identity, plus the joystick sources
    // (#1061).
    static constexpr int kFormatVersion = 3;

    // The `version` value of a stored file, or 0 when it predates versioning. Callers regenerate
    // the file when this is below kFormatVersion.
    [[nodiscard]] static int fileFormatVersion(const std::string& toml);

    // One pair of actions that would both fire from the same physical input in the same session
    // mode. Reported by findConflicts(), which is what holds applyDefaults() to the same rule a
    // user rebind has always been held to.
    struct Conflict {
        InputAction a{};
        int indexA{0}; // position in a's binding list
        InputAction b{};
        int indexB{0};
        Binding binding{};
    };

    // A distinct concrete device the table refers to, and how many bindings depend on it. The caller
    // decides what to do about one that is not plugged in — see the absent-device rule in
    // docs/user-guide/controls.md.
    struct DeviceUse {
        DeviceRef ref;
        std::string name; // from the [[devices]] table; empty when never seen
        int bindingCount{0};
    };

    InputBindings();

    void applyDefaults();

    // Every binding for this action, in priority order. Empty = the action is unbound.
    [[nodiscard]] std::span<const Binding> get(InputAction action) const;

    // The first binding, or a None binding when the action is unbound. For labels and for tests that
    // only care that an action has its expected default.
    [[nodiscard]] Binding first(InputAction action) const;

    [[nodiscard]] int count(InputAction action) const;

    // Replaces the whole list. None bindings are dropped, so callers can hand over a fixed-size array.
    void set(InputAction action, std::span<const Binding> bindings);

    // Appends. Returns false when the binding is None or the action already has an identical one —
    // a duplicate would fire the same control twice and show up as a conflict with itself.
    bool add(InputAction action, const Binding& binding);

    // Erases every exactly-equal binding. Returns true when something was removed.
    bool remove(InputAction action, const Binding& binding);

    void clear(InputAction action);

    // The session modes in which the game reads this action. Two actions can share a binding
    // without conflicting when their context sets are disjoint — see InputContext.
    [[nodiscard]] static InputContext contexts(InputAction action) noexcept;

    [[nodiscard]] static const char* actionName(InputAction action);
    [[nodiscard]] static std::optional<InputAction> actionFromName(const std::string& name);

    // --- Device registry -----------------------------------------------------

    // Record (or refresh) the human-readable name for a device GUID. Called from the live device list
    // at startup and on hot-plug, so the file always carries current names.
    void noteDevice(const DeviceRef& ref, std::string name);

    // "" when the GUID has never been seen.
    [[nodiscard]] const char* deviceName(const DeviceRef& ref) const;

    [[nodiscard]] std::span<const KnownDevice> devices() const noexcept {
        return m_devices;
    }

    // Every distinct concrete device named by at least one binding, with its binding count. Wildcard
    // ("any device") bindings are not listed: they are not waiting for a particular stick.
    [[nodiscard]] std::vector<DeviceUse> deviceUsage() const;

    // --- Conflicts -----------------------------------------------------------

    // Returns the first action that already uses the given binding IN AN OVERLAPPING CONTEXT
    // (excluding 'skipAction' so you can re-assign an action's own binding without a false
    // positive). Returns nullopt if no conflict.
    [[nodiscard]] std::optional<InputAction> conflictsWith(InputAction skipAction, const Binding& binding) const;

    // Every conflicting pair in the whole table, lowest action ordinal first. Empty = the table is
    // self-consistent. Used by the defaults test and by the game at startup to warn about a
    // hand-edited bindings.toml.
    [[nodiscard]] std::vector<Conflict> findConflicts() const;

    // --- Serialization -------------------------------------------------------

    // Serializes the device registry and every action's binding list to a TOML string.
    [[nodiscard]] std::string serialize() const;

    // Parses a TOML string into the binding table. Accepts the version-3 `[bindings]` array-of-tables
    // form and, so an existing install is not silently reset, the version-2
    // `[primary]`/`[secondary]`/`[gamepad]` (and version-1 `[alt]`) sections. On parse failure the
    // existing state is unchanged and false is returned.
    bool deserialize(const std::string& toml);

  private:
    std::array<std::vector<Binding>, static_cast<std::size_t>(kActionCount)> m_bindings{};
    std::vector<KnownDevice> m_devices;

    static std::string serializeBinding(const Binding& b);
};

} // namespace fl
