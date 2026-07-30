// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Binding.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fl {

enum class AxisCurve : uint8_t {
    Linear,
    Cubic,
};

// How the physical axis rests.
//
// A spring-return stick or a gamepad thumbstick is CENTERED: zero is the middle of its travel, the
// value IS the control deflection, and a centre deadzone is what keeps a worn potentiometer from
// holding a permanent turn. A HOTAS throttle lever is ABSOLUTE: it stays where you leave it, its full
// travel maps onto [0, 1], and a centre deadzone would carve a dead notch out of the middle of the
// throttle range.
//
// Before #1061 this distinction was hardcoded — the HOTAS throttle got the (raw + 1) / 2 remap in a
// parallel code path in FlightInputCollector, and the docs had to tell players that `invert` "is not
// meaningful" for a trigger axis. It is a property of the AXIS, so it belongs on the axis's config.
enum class AxisMode : uint8_t {
    Centered,
    Absolute,
};

// One processed axis reading.
//
// `active` answers "is this axis driving the control right now?", which is a different question from
// "is the value non-zero". An absolute throttle lever parked at idle reads 0.0 and is still very much
// in command — treating that as inactive is how a keyboard throttle would fight a HOTAS that the
// player had deliberately closed.
struct AxisSample {
    float value{0.0f};
    bool active{false};
};

struct AxisConfig {
    float deadzone{0.1f}; // [0.0, 1.0]; Centered only — an absolute lever has no centre
    AxisCurve curve{AxisCurve::Linear};
    AxisMode mode{AxisMode::Centered};
    bool invert{false};
    float scale{1.0f};

    // Centered: deadzone → rescale → curve → invert → scale, active past the deadzone.
    // Absolute: invert → [-1, 1] remapped to [0, 1] → scale, always active.
    [[nodiscard]] AxisSample apply(float raw) const;
};

// The standard HOTAS axis layout, shipped as the default for `any` joystick. These are the indices
// the pre-#1061 `[controls] hotas_*_axis` keys defaulted to; `kDefaults` in InputBindings.cpp binds
// the same four, and applyDefaults() below tunes them, so the two halves of the shipped HOTAS
// mapping are stated once each and read the same numbers.
inline constexpr uint32_t kHotasAxisRoll = 0;
inline constexpr uint32_t kHotasAxisPitch = 1;
inline constexpr uint32_t kHotasAxisThrottle = 2;
inline constexpr uint32_t kHotasAxisYaw = 3;
// 0.05, not the gamepad's 0.1: a HOTAS potentiometer has far less slop than a thumbstick, and this
// was the pre-#1061 `hotas_deadzone` default.
inline constexpr float kHotasDefaultDeadzone = 0.05f;

// Identifies one physical axis: which class of device, WHICH device, and which axis on it.
//
// The pre-#1061 table was a fixed array of six entries indexed by GamepadAxis, so "joystick 2, axis 9"
// was not expressible — and neither was giving two identical sticks different curves. Keying on the
// same (source, device, index) triple a Binding carries means an axis is configured by the same handle
// it is bound by.
struct AxisKey {
    BindingSource source{BindingSource::GamepadAxis};
    uint32_t index{0};  // GamepadAxis ordinal, or the raw joystick axis index
    DeviceRef device{}; // joystick axes only; empty = any device

    [[nodiscard]] constexpr bool operator==(const AxisKey& o) const noexcept {
        return source == o.source && index == o.index && sameDeviceRef(device, o.device);
    }
};

// The key of the axis a binding reads, or nullopt when the binding is not an axis.
[[nodiscard]] constexpr AxisKey axisKeyOf(const Binding& b) noexcept {
    return AxisKey{b.source, b.id, b.device};
}

// Per-axis deadzone / curve / mode / inversion / scale, keyed by physical axis.
//
// Serialized as an `[[axis_config]]` array of tables in bindings.toml. A version-2 file's
// `[axis_config]` TABLE (keyed by gamepad axis name) is still read, because a player's axis tuning must
// survive the format move — the whole point of the version field is that a schema change is cheap for
// us and free for them.
class AxisConfigTable {
  public:
    struct Entry {
        AxisKey key;
        AxisConfig config;
    };

    AxisConfigTable();

    // The shipped tuning: the six gamepad axes, plus the four default HOTAS axes migrated out of
    // `[controls]` in user.toml (#1061), whose 0.05 deadzone and absolute-throttle behaviour were the
    // pre-#1061 hotas* semantics.
    void applyDefaults();

    // The stored config for this axis, or a default-constructed AxisConfig when it has none. Callers
    // want a config for every axis a device happens to have, so a miss is a default, not an error.
    [[nodiscard]] AxisConfig effective(const AxisKey& key) const;

    [[nodiscard]] const AxisConfig* find(const AxisKey& key) const;
    void set(const AxisKey& key, const AxisConfig& config);
    bool erase(const AxisKey& key);

    [[nodiscard]] std::span<const Entry> entries() const noexcept {
        return m_entries;
    }

    // Serializes every entry as an `[[axis_config]]` array-of-tables block.
    [[nodiscard]] std::string serialize() const;

    // Parses `[[axis_config]]` (version 3) or a legacy `[axis_config]` table (version 2) out of the
    // given TOML. On parse failure the existing state is unchanged and false is returned.
    bool deserialize(const std::string& toml);

  private:
    std::vector<Entry> m_entries;
};

} // namespace fl
