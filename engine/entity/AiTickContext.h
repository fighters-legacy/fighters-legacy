// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

class SpatialIndex; // engine/spatial/SpatialIndex.h
struct AiScaling;   // engine/config/DifficultySettings.h

namespace sensor {
struct ContactTable;       // engine/sensor/SensorSystem.h (#685)
struct SensingEnvironment; // engine/sensor/Detection.h
} // namespace sensor

// Everything a controller is allowed to know about the world this tick, in one place.
//
// This replaces the `const SpatialIndex* si` tail on IEntityController::sample(). The seam was
// about to grow three more parameters — contacts, environment, difficulty — and every one of them
// would have meant touching all thirteen controllers, the Lua bridge, the state machine, the
// factory and every test again. Bundling costs one indirection and buys a signature that stops
// changing.
//
// EVERY FIELD IS A NULLABLE POINTER, and null is a meaningful, normative value: it means "this was
// not evaluated in the context you were called from". A controller unit test constructs
// `AiTickContext{}` and gets exactly the behavior it had before any of this existed. That is what
// makes this refactor a no-op today and a seam tomorrow.
//
// NOTE the header this lives in. `engine-entity` must NOT depend on `engine-sensor` — the observed
// does not depend on the observer (2026-07-12 decision record) — so the sensor types are FORWARD
// DECLARED here, exactly as `SpatialIndex` already was in IEntityController.h. A controller that
// actually reads contacts includes the sensor header itself and links `engine-sensor`; a controller
// that does not, does not pay for it.
struct AiTickContext {
    // The spatial index rebuilt at the start of this tick by WorldBroadcaster. Non-null when called
    // from the broadcaster; null in tests and other contexts.
    const SpatialIndex* si{nullptr};

    // THIS ENTITY'S CONTACTS — what it has honestly detected, never ground truth (#685).
    //
    // Null means sensing was not evaluated, NOT "this entity sees nothing". The distinction is
    // load-bearing: a null table preserves the pre-sensing behavior (a controller that reads the
    // EntityManager directly), while an EMPTY table means the sensors ran and found nothing. Once
    // the sensing pass lands, a controller that consults `contacts` cannot see through terrain or
    // across the map, because the ground truth is not reachable from here.
    const sensor::ContactTable* contacts{nullptr};

    // Weather / time-of-day as the sensors see it. Null = not evaluated.
    const sensor::SensingEnvironment* env{nullptr};

    // The active difficulty scaling (reaction time, aim error, radar range…). Null = not evaluated,
    // which every controller must read as "no scaling", not as "zero".
    const AiScaling* difficulty{nullptr};
};

} // namespace fl
