// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// The builtin placeholder silhouette vocabulary (#886) — the RENDER-side key a mesh-less entity
// type resolves to. Deliberately its own tiny header (not BuiltinGeometry.h) so the entity-layer
// mapping (engine/entity/BuiltinShapeMap.h) can include it without dragging render headers along;
// deliberately NOT ObjectCategory — engine-render never includes engine-entity headers, and the
// game-layer resolver owns the entity-vocabulary -> render-vocabulary mapping.
enum class BuiltinShape : uint8_t {
    Unknown, // spiky jack/caltrop error beacon — renders ONLY in bug states (type not in the
             // client registry, the never-spawned Effect category, an unmapped ordinal). A bug
             // must not look like a plausible aircraft (#832's lesson) — seeing this IS the signal.
    AirVehicle,
    Missile,
    Bomb,
    Rocket,
    GroundVehicle,
    NavalVessel,
    Structure,
    Parachute, // ejected-pilot canopy (#672): the one Effect-category entity that replicates and
               // must render (a hanging chute), so Effect no longer maps to the Unknown beacon.
    Count,
};

} // namespace fl
