// SPDX-License-Identifier: GPL-3.0-or-later
#include "AxisConfig.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <toml++/toml.hpp>

namespace fl {

// ---------------------------------------------------------------------------
// AxisConfig::apply
// ---------------------------------------------------------------------------

float AxisConfig::apply(float raw) const {
    float magnitude = std::abs(raw);
    float sign = (raw < 0.0f) ? -1.0f : 1.0f;

    if (magnitude < deadzone)
        return 0.0f;

    // Rescale from [deadzone, 1.0] to [0.0, 1.0]
    float t = (magnitude - deadzone) / (1.0f - deadzone);
    t = std::min(1.0f, std::max(0.0f, t));

    switch (curve) {
    case AxisCurve::Cubic:
        t = t * t * t;
        break;
    case AxisCurve::Linear:
    default:
        break;
    }

    float result = sign * t * scale;
    return invert ? -result : result;
}

// ---------------------------------------------------------------------------
// AxisConfigTable
// ---------------------------------------------------------------------------

static constexpr const char* kAxisNames[] = {
    "LeftX", "LeftY", "RightX", "RightY", "TriggerLeft", "TriggerRight",
};
static_assert(std::size(kAxisNames) == static_cast<size_t>(GamepadAxis::Count),
              "kAxisNames must have one entry per GamepadAxis");

AxisConfig& AxisConfigTable::get(GamepadAxis axis) {
    return m_configs[static_cast<int>(axis)];
}

const AxisConfig& AxisConfigTable::get(GamepadAxis axis) const {
    return m_configs[static_cast<int>(axis)];
}

std::string AxisConfigTable::serialize() const {
    std::ostringstream out;
    out << "[axis_config]\n";
    for (int i = 0; i < kAxisCount; ++i) {
        const AxisConfig& c = m_configs[i];
        out << kAxisNames[i] << " = { deadzone = " << c.deadzone << ", curve = \""
            << (c.curve == AxisCurve::Cubic ? "Cubic" : "Linear") << "\""
            << ", invert = " << (c.invert ? "true" : "false") << ", scale = " << c.scale << " }\n";
    }
    return out.str();
}

bool AxisConfigTable::deserialize(const std::string& toml) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml);
    } catch (const toml::parse_error&) {
        return false;
    }

    auto* sec = tbl["axis_config"].as_table();
    if (!sec)
        return true; // absent section is fine; keep defaults

    auto tmpConfigs = m_configs;
    for (int i = 0; i < kAxisCount; ++i) {
        auto* entry = (*sec)[kAxisNames[i]].as_table();
        if (!entry)
            continue;
        AxisConfig& c = tmpConfigs[i];
        if (auto v = entry->get("deadzone"))
            c.deadzone = v->value_or(c.deadzone);
        if (auto v = entry->get("invert"))
            c.invert = v->value_or(c.invert);
        if (auto v = entry->get("scale"))
            c.scale = v->value_or(c.scale);
        if (auto v = entry->get("curve")) {
            auto curveStr = v->value_or(std::string{});
            if (curveStr == "Cubic")
                c.curve = AxisCurve::Cubic;
            if (curveStr == "Linear")
                c.curve = AxisCurve::Linear;
        }
    }
    m_configs = tmpConfigs;
    return true;
}

} // namespace fl
