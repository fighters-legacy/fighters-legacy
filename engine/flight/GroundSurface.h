// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// Per-surface ground-handling parameters the FlightIntegrator applies during ground contact (#487).
// A plain POD with no dependency on the SurfaceType vocabulary (which lives in the higher engine-render
// layer): the caller maps its surface to a GroundFriction (see engine/render/SurfaceType.h
// groundFrictionFor) and passes it to step(). The default is a hard paved surface — zero EXTRA rolling
// resistance beyond the integrator's baseline ground roll — so a step() call that omits it is
// bit-identical to before this feature.
struct GroundFriction {
    // Extra rolling deceleration (fraction of horizontal ground speed shed per second) ON TOP of the
    // integrator's baseline. 0 = a hard paved runway (concrete/asphalt). Grass and gravel add drag, so
    // a rollout on grass is shorter than on concrete for the same touchdown speed.
    float extraRollingPerSec{0.f};
};

} // namespace fl
