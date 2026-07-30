// SPDX-License-Identifier: GPL-3.0-or-later
#include "LegacyHotas.h"
#include <vector>

namespace fl {

namespace {

// One migrated axis: which action it drives, the legacy axis index, whether the old config inverted
// it, and whether it is a full-travel lever rather than a spring-return stick.
struct AxisMove {
    InputAction action;
    int index;
    bool invert;
    AxisMode mode;
};

void applyMove(const AxisMove& m, float deadzone, InputBindings& bindings, AxisConfigTable& axes) {
    // Rebuild the list with the migrated joystick axis first, dropping any joystick axis already there.
    // Dropping is what makes a repeated migration idempotent — and it is also how index -1 is honoured:
    // -1 meant "this axis is switched off", so the shipped default binding has to GO, not stay.
    std::vector<Binding> list;
    Binding b{};
    b.source = BindingSource::JoystickAxis;
    b.id = static_cast<uint32_t>(m.index);
    if (m.index >= 0)
        list.push_back(b);
    for (const Binding& existing : bindings.get(m.action))
        if (existing.source != BindingSource::JoystickAxis)
            list.push_back(existing);
    bindings.set(m.action, list);

    if (m.index < 0)
        return;

    AxisConfig cfg;
    cfg.deadzone = deadzone;
    cfg.invert = m.invert;
    cfg.mode = m.mode;
    axes.set(AxisKey{BindingSource::JoystickAxis, b.id, DeviceRef{}}, cfg);
}

} // namespace

void migrateLegacyHotas(const LegacyHotasAxes& legacy, InputBindings& bindings, AxisConfigTable& axes) {
    // With nothing stored, the shipped layout is the migration: the axes move into the table at their
    // default indices with their default tuning.
    const LegacyHotasAxes l = legacy.present ? legacy : LegacyHotasAxes{};

    const AxisMove moves[] = {
        {InputAction::RollAxis, l.aileronAxis, l.invertRoll, AxisMode::Centered},
        {InputAction::PitchAxis, l.elevatorAxis, l.invertPitch, AxisMode::Centered},
        {InputAction::YawAxis, l.rudderAxis, l.invertRudder, AxisMode::Centered},
        // The throttle is the only ABSOLUTE one: the old code remapped its full [-1, 1] travel onto
        // [0, 1] and never applied the deadzone to it, which is exactly AxisMode::Absolute.
        {InputAction::ThrottleAxis, l.throttleAxis, l.invertThrottle, AxisMode::Absolute},
    };
    for (const AxisMove& m : moves)
        applyMove(m, l.deadzone, bindings, axes);
}

} // namespace fl
