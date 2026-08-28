// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Binding.h"
#include <span>
#include <string>
#include <vector>

namespace fl {

// Resolves a binding's DeviceRef to the live IJoystick index, and derives hat edges (#1061).
//
// Two jobs, both of which have to happen at a frame boundary, which is why they are one object:
//
//  1. GUID -> INDEX. Bindings are stored by GUID because SDL renumbers indices on hot-plug; the HAL is
//     addressed by index. Something has to hold the mapping, and it has to be re-derived before any
//     read in a frame where a device was added or removed — SDL3Joystick erases the removed device from
//     its vector, so every index at or above it shifts.
//
//  2. HAT EDGES. IJoystick reports a hat's POSITION and offers no just-pressed flag for it (unlike
//     buttons). Without an edge, a hat could only ever drive held controls, and a player who bound the
//     landing gear to their POV hat would find it does nothing. The previous position is kept per
//     device GUID, not per index, so a hot-plug does not manufacture a spurious edge on an unrelated
//     stick.
//
// update() is a full reconcile rather than a subscription to IJoystickEventHandler: a reconcile cannot
// miss an event or run against a stale index, needs no handler lifetime, and the per-frame call has to
// exist anyway for the hat edges. Call it once per frame at the same boundary as IJoystick::flush().
class JoystickDevices {
  public:
    static constexpr int kAbsent = -1;

    // One device appearing or disappearing since the previous update(). Drained by the caller for
    // logging — a control that stopped working with no signal is the #1050 experience again.
    struct Change {
        std::string guid;
        std::string name;
        bool added{false};
    };

    // Reconcile against the live device set. Safe to call with no devices, and safe to call every
    // frame — the cost is a handful of string compares.
    void update(const class IJoystick& js);

    // Present devices, in live index order.
    struct Device {
        std::string guid;
        std::string name;
        int index{kAbsent};
    };
    [[nodiscard]] std::span<const Device> present() const noexcept {
        return m_devices;
    }

    // The live index for this ref, or kAbsent when the device is not connected. An `Any` ref resolves
    // to the first present device — a single representative index; the binding queries scan every
    // present device for `Any` themselves (#1358).
    [[nodiscard]] int resolve(const DeviceRef& ref) const;

    [[nodiscard]] bool isPresent(const DeviceRef& ref) const {
        return resolve(ref) != kAbsent;
    }

    // Rising edge of a hat direction on a live device index, derived from the position observed by the
    // previous update(). `hatMatches` semantics: a cardinal binding also fires on its two diagonals.
    [[nodiscard]] bool hatJustPressed(int deviceIndex, uint32_t hatIndex, HatPosition direction) const;

    // Devices added / removed by the most recent update(). Replaced on each update().
    [[nodiscard]] std::span<const Change> changes() const noexcept {
        return m_changes;
    }

    // Bumped by update() whenever the device set changes. Consumers holding per-device state at a
    // DIFFERENT cadence than update() — AxisMotionTracker polls at 60 Hz while this reconciles per
    // frame — cannot watch changes() (it is replaced every frame, so a change between two polls is
    // invisible); a monotonic counter cannot be missed (#1358).
    [[nodiscard]] uint64_t generation() const noexcept {
        return m_generation;
    }

  private:
    struct HatState {
        std::string guid;
        std::vector<HatPosition> previous;
        std::vector<HatPosition> current;
    };

    std::vector<Device> m_devices;
    std::vector<HatState> m_hats; // paired to devices by GUID with claimed-consumption (see update())
    std::vector<Change> m_changes;
    uint64_t m_generation{0};

    [[nodiscard]] const HatState* hatStateFor(int deviceIndex) const;
};

} // namespace fl
