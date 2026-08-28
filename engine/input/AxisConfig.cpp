// SPDX-License-Identifier: GPL-3.0-or-later
#include "AxisConfig.h"
#include "InputNames.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <toml++/toml.hpp>

namespace fl {

// ---------------------------------------------------------------------------
// AxisConfig::apply
// ---------------------------------------------------------------------------

AxisSample AxisConfig::apply(float raw) const {
    if (mode == AxisMode::Absolute) {
        // A full-travel lever: the whole [-1, 1] sweep maps onto [0, 1] and there is no centre to
        // ignore. `active` here is PROVISIONAL — there is no rest position to gate on, so the value
        // alone cannot say whether the lever is what the player is using. actionAxis() gates it on
        // motion instead (#1358): declaring it always in command is how an unfitted channel stuck at
        // −1.0 latched the throttle at idle and locked the keyboard out.
        const float r = invert ? -raw : raw;
        const float v = std::clamp((r + 1.0f) * 0.5f, 0.0f, 1.0f) * scale;
        return {v, true};
    }

    const float magnitude = std::abs(raw);
    const float sign = (raw < 0.0f) ? -1.0f : 1.0f;

    // `<=`, so a raw value sitting exactly on the deadzone (and, with deadzone 0, a stick at rest)
    // reports INACTIVE rather than an active zero — an active zero would claim the control and stop the
    // keyboard from driving it.
    if (magnitude <= deadzone)
        return {0.0f, false};

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

    const float result = sign * t * scale;
    return {invert ? -result : result, true};
}

// ---------------------------------------------------------------------------
// AxisConfigTable
// ---------------------------------------------------------------------------

namespace {

const char* curveName(AxisCurve c) {
    return c == AxisCurve::Cubic ? "Cubic" : "Linear";
}
const char* modeName(AxisMode m) {
    return m == AxisMode::Absolute ? "Absolute" : "Centered";
}

// The pre-#1061 `[axis_config]` section keyed its six entries by gamepad axis NAME. Reading that form
// is what keeps a version-2 file's tuning when the table is regenerated at version 3.
constexpr GamepadAxis kLegacyAxisOrder[] = {
    GamepadAxis::LeftX,  GamepadAxis::LeftY,       GamepadAxis::RightX,
    GamepadAxis::RightY, GamepadAxis::TriggerLeft, GamepadAxis::TriggerRight,
};

AxisKey gamepadKey(GamepadAxis a) {
    return AxisKey{BindingSource::GamepadAxis, static_cast<uint32_t>(a), DeviceRef{}};
}
AxisKey joystickKey(uint32_t index) {
    return AxisKey{BindingSource::JoystickAxis, index, DeviceRef{}};
}

void readFields(const toml::table& entry, AxisConfig& c) {
    if (auto v = entry.get("deadzone"))
        c.deadzone = std::clamp(v->value_or(c.deadzone), 0.0f, 0.99f);
    if (auto v = entry.get("invert"))
        c.invert = v->value_or(c.invert);
    if (auto v = entry.get("scale"))
        c.scale = v->value_or(c.scale);
    if (auto v = entry.get("curve")) {
        const auto s = v->value_or(std::string{});
        if (s == "Cubic")
            c.curve = AxisCurve::Cubic;
        else if (s == "Linear")
            c.curve = AxisCurve::Linear;
    }
    if (auto v = entry.get("mode")) {
        const auto s = v->value_or(std::string{});
        if (s == "Absolute")
            c.mode = AxisMode::Absolute;
        else if (s == "Centered")
            c.mode = AxisMode::Centered;
    }
}

} // namespace

AxisConfigTable::AxisConfigTable() {
    applyDefaults();
}

void AxisConfigTable::applyDefaults() {
    m_entries.clear();
    for (GamepadAxis a : kLegacyAxisOrder)
        set(gamepadKey(a), AxisConfig{});

    // The four default HOTAS axes, migrated out of `[controls]` (#1061). Their old semantics land here
    // exactly: a 0.05 deadzone on stick and pedals, and an ABSOLUTE throttle lever — which used to be a
    // hardcoded (raw + 1) / 2 in the collector rather than anything a player could see or change.
    AxisConfig stick;
    stick.deadzone = kHotasDefaultDeadzone;
    set(joystickKey(kHotasAxisRoll), stick);
    set(joystickKey(kHotasAxisPitch), stick);
    set(joystickKey(kHotasAxisYaw), stick);

    AxisConfig lever;
    lever.deadzone = kHotasDefaultDeadzone;
    lever.mode = AxisMode::Absolute;
    set(joystickKey(kHotasAxisThrottle), lever);
}

const AxisConfig* AxisConfigTable::find(const AxisKey& key) const {
    for (const auto& e : m_entries)
        if (e.key == key)
            return &e.config;
    return nullptr;
}

AxisConfig AxisConfigTable::effective(const AxisKey& key) const {
    if (const AxisConfig* c = find(key))
        return *c;
    // An exact miss falls back to the same axis on ANY device: a player who tuned "joystick axis 2"
    // without naming a GUID meant it for whatever stick is plugged in, and binding that axis to a
    // named device must not silently drop the tuning they already did.
    if (!key.device.isAny()) {
        if (const AxisConfig* c = find(AxisKey{key.source, key.index, DeviceRef{}}))
            return *c;
    }
    return AxisConfig{};
}

void AxisConfigTable::set(const AxisKey& key, const AxisConfig& config) {
    for (auto& e : m_entries) {
        if (e.key == key) {
            e.config = config;
            return;
        }
    }
    m_entries.push_back({key, config});
}

bool AxisConfigTable::erase(const AxisKey& key) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.key == key; });
    if (it == m_entries.end())
        return false;
    m_entries.erase(it);
    return true;
}

std::string AxisConfigTable::serialize() const {
    std::ostringstream out;
    out << "# Per-axis tuning, keyed by the same (source, device, index) triple a binding uses.\n"
           "# source: \"GamepadAxis\" (id = the axis name) or \"JoystickAxis\" (index = the raw axis number).\n"
           "# device: a joystick GUID from the [[devices]] table; omit or leave empty for \"any stick\".\n"
           "# mode:   \"Centered\" for a spring-return stick, \"Absolute\" for a throttle lever that stays put.\n";
    for (const auto& e : m_entries) {
        out << "\n[[axis_config]]\n";
        out << "source = \"" << bindingSourceName(e.key.source) << "\"\n";
        if (e.key.source == BindingSource::GamepadAxis) {
            const char* n = gamepadAxisName(static_cast<GamepadAxis>(e.key.index));
            out << "id = \"" << (n ? n : "LeftX") << "\"\n";
        } else {
            out << "index = " << e.key.index << "\n";
            if (!e.key.device.isAny())
                out << "device = \"" << e.key.device.guid << "\"\n";
        }
        out << "deadzone = " << e.config.deadzone << "\n";
        out << "curve = \"" << curveName(e.config.curve) << "\"\n";
        out << "mode = \"" << modeName(e.config.mode) << "\"\n";
        out << "invert = " << (e.config.invert ? "true" : "false") << "\n";
        out << "scale = " << e.config.scale << "\n";
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

    auto node = tbl["axis_config"];
    if (!node)
        return true; // absent section is fine; keep whatever we already hold

    // Parse into a scratch copy so a mid-parse failure leaves the live table untouched.
    AxisConfigTable scratch;
    scratch.m_entries = m_entries;

    if (auto* arr = node.as_array()) {
        // Version 3: one [[axis_config]] block per physical axis.
        for (const auto& elem : *arr) {
            const auto* entry = elem.as_table();
            if (!entry)
                continue;
            const auto srcName = entry->get("source") ? entry->get("source")->value_or(std::string{}) : std::string{};
            const auto src = bindingSourceFromName(srcName);
            if (!src || !isAxisSource(*src))
                return false;
            AxisKey key{};
            key.source = *src;
            if (*src == BindingSource::GamepadAxis) {
                const auto idName = entry->get("id") ? entry->get("id")->value_or(std::string{}) : std::string{};
                const auto ax = gamepadAxisFromName(idName);
                if (!ax)
                    return false;
                key.index = static_cast<uint32_t>(*ax);
            } else {
                const auto idx = entry->get("index") ? entry->get("index")->value_or(-1LL) : -1LL;
                if (idx < 0)
                    return false;
                key.index = static_cast<uint32_t>(idx);
                const auto dev = entry->get("device") ? entry->get("device")->value_or(std::string{}) : std::string{};
                key.device = makeDeviceRef(dev.c_str());
            }
            AxisConfig c = scratch.effective(key);
            readFields(*entry, c);
            scratch.set(key, c);
        }
    } else if (auto* sec = node.as_table()) {
        // Version 2: an [axis_config] table keyed by gamepad axis name, six entries, gamepad only.
        for (GamepadAxis a : kLegacyAxisOrder) {
            const char* n = gamepadAxisName(a);
            const auto* entry = n ? (*sec)[n].as_table() : nullptr;
            if (!entry)
                continue;
            const AxisKey key = gamepadKey(a);
            AxisConfig c = scratch.effective(key);
            readFields(*entry, c);
            scratch.set(key, c);
        }
    } else {
        return false;
    }

    m_entries = std::move(scratch.m_entries);
    return true;
}

} // namespace fl
