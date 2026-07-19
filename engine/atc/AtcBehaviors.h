// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/HoldShortController.h"
#include "ai/LandingController.h"
#include "ai/LoiterController.h"
#include "ai/StateMachineController.h"
#include "ai/TakeoffController.h"
#include "atc/AtcService.h"
#include "entity/EntityId.h"

#include <glm/glm.hpp>
#include <memory>

namespace fl {
class EntityManager;
} // namespace fl

namespace fl::atc {

// The runway/pattern geometry an ATC composition needs. Built from a facility's Runway + field pose.
struct AtcRunwaySpec {
    glm::dvec3 threshold{0.0};   // runway threshold (departure/landing end)
    glm::dvec3 fieldCenter{0.0}; // field reference point (pattern-loiter centre)
    float headingDeg{90.f};      // runway heading
    float runwayElevM{0.f};      // field elevation (MSL)
    float climboutAglM{300.f};   // AGL at which a departure is handed off
    float patternAltM{600.f};    // arrival pattern-loiter altitude (MSL)
    float glideslopeDeg{3.5f};
    float approachSpeedMps{75.f};
    float rotateSpeedMps{70.f};
};

// Condition: this flight currently holds a ClearedTakeoff clearance from the service.
[[nodiscard]] inline fl::ai::Condition ClearedForTakeoff(AtcService& svc, fl::EntityId id) {
    return [&svc, id](const fl::EntityState&, const fl::EntityManager&, const fl::AiTickContext&) {
        return svc.clearanceState(id) == ClearanceState::ClearedTakeoff;
    };
}

// Condition: this flight currently holds a ClearedToLand clearance.
[[nodiscard]] inline fl::ai::Condition ClearedToLand(AtcService& svc, fl::EntityId id) {
    return [&svc, id](const fl::EntityState&, const fl::EntityManager&, const fl::AiTickContext&) {
        return svc.clearanceState(id) == ClearanceState::ClearedToLand;
    };
}

// Condition: this flight was waved off (go around).
[[nodiscard]] inline fl::ai::Condition ToldToGoAround(AtcService& svc, fl::EntityId id) {
    return [&svc, id](const fl::EntityState&, const fl::EntityManager&, const fl::AiTickContext&) {
        return svc.clearanceState(id) == ClearanceState::GoAround;
    };
}

// A departure composition: hold short -> (cleared) take off -> (climbed out) hand off to nextFactory.
// nextFactory produces the controller that flies the aircraft once airborne (e.g. a patrol/loiter).
[[nodiscard]] inline std::unique_ptr<fl::IEntityController>
makeAtcDepartureController(AtcService& svc, fl::EntityId id, const fl::EntityManager& em, const AtcRunwaySpec& spec,
                           fl::ai::ControllerFactory nextFactory) {
    auto sm = std::make_unique<fl::ai::StateMachineController>(em);
    sm->addState("hold_short", [] { return std::make_unique<fl::ai::HoldShortController>(); });
    sm->addState("takeoff", [spec] {
        return std::make_unique<fl::ai::TakeoffController>(spec.threshold, spec.headingDeg, spec.runwayElevM,
                                                           spec.rotateSpeedMps, spec.climboutAglM);
    });
    sm->addState("depart", std::move(nextFactory));
    sm->addTransition("hold_short", "takeoff", ClearedForTakeoff(svc, id));
    sm->addTransition("takeoff", "depart", fl::ai::AboveAltitude(spec.runwayElevM + spec.climboutAglM));
    sm->setInitialState("hold_short");
    return sm;
}

// An arrival composition: loiter the pattern -> (cleared) fly the approach + land; a go-around returns
// to the pattern to try again.
[[nodiscard]] inline std::unique_ptr<fl::IEntityController>
makeAtcArrivalController(AtcService& svc, fl::EntityId id, const fl::EntityManager& em, const AtcRunwaySpec& spec) {
    auto sm = std::make_unique<fl::ai::StateMachineController>(em);
    sm->addState("pattern", [spec] {
        return std::make_unique<fl::ai::LoiterController>(spec.fieldCenter, 3000.f, spec.patternAltM, 0.6f);
    });
    sm->addState("final", [spec] {
        return std::make_unique<fl::ai::LandingController>(spec.threshold, spec.headingDeg, spec.runwayElevM,
                                                           spec.glideslopeDeg, spec.approachSpeedMps);
    });
    sm->addTransition("pattern", "final", ClearedToLand(svc, id));
    // A wave-off returns to the pattern; a short dwell keeps it from re-triggering on the same tick.
    sm->addTransition("final", "pattern", ToldToGoAround(svc, id), 1.0f);
    sm->setInitialState("pattern");
    return sm;
}

} // namespace fl::atc
