// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IInput.h"
#include <array>
#include <string>

namespace fl {

enum class AxisCurve : uint8_t {
    Linear,
    Cubic,
};

struct AxisConfig {
    float deadzone{0.1f}; // [0.0, 1.0]; input magnitude below this maps to 0.0
    AxisCurve curve{AxisCurve::Linear};
    bool invert{false};
    float scale{1.0f};

    // Applies deadzone → rescale → curve → invert → scale in order.
    float apply(float raw) const;
};

// One AxisConfig per GamepadAxis value.
class AxisConfigTable {
  public:
    static constexpr int kAxisCount = static_cast<int>(GamepadAxis::Count);

    AxisConfig& get(GamepadAxis axis);
    const AxisConfig& get(GamepadAxis axis) const;

    // Serializes all axis configs to TOML. Intended to be embedded under an
    // [axis_config] section alongside the bindings TOML produced by InputBindings.
    std::string serialize() const;

    // Parses an [axis_config] section from the given TOML string. On parse failure
    // the existing state is unchanged and false is returned.
    bool deserialize(const std::string& toml);

  private:
    std::array<AxisConfig, kAxisCount> m_configs{};
};

} // namespace fl
