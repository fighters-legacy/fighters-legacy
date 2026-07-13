// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/IEntityController.h"

#include <cstdint>
#include <glm/glm.hpp>

namespace fl {
class EntityManager;
}

namespace fl::ai {

// Geometry of a formation, independent of how many aircraft are in it.
//
// The unit of organisation is the FLIGHT, not "the wingman" — a flight is a lead plus N members, and
// N is 1 or 6+ with equal validity. "Wingman" is the radio vocabulary (WingmanCommand.h), not the
// data model. So a slot is an INDEX, not one of a fixed set of named positions: `slotIndex` 0 is the
// first member, 1 the second, and the geometry below generates a sane station for any index without
// a table to run out of.
struct FormationParams {
    float lateralM = 150.f;  // lateral spacing per rank (see formationSlotOffset); 0 = line astern
    float aftM = 100.f;      // aft spacing per rank
    float verticalM = -15.f; // vertical spacing per rank along LOCAL up; negative = stepped down
    float throttleBase = 0.65f;
    float rangeGain = 0.00008f; // throttle per metre of along-track slot error
    float closureGain = 0.02f;  // throttle per m/s of closure rate (damps the chase)
    float minThrottle = 0.05f;
    float maxThrottle = 1.f;
    float afterburnerErrorM = 2000.f; // slot error above which AB lights, to make a rejoin from far out
};

// Slot offset in the LEAD's frame: {right, forward, up} metres, for any slot index.
//
// Members alternate right/left and step out+back+down each rank, so a flight of any size stacks into
// a legible echelon:
//   slot 0 -> ( +lateral, -aft, +vertical)      slot 1 -> ( -lateral, -aft, +vertical)
//   slot 2 -> (+2*lateral, -2*aft, +2*vertical) slot 3 -> (-2*lateral, ...)   ... and so on.
// It is a plain echelon rather than a doctrinal finger-four: honest, unbounded in N, and cheap to
// replace once formations are authorable content.
[[nodiscard]] glm::vec3 formationSlotOffset(uint32_t slotIndex, const FormationParams& params) noexcept;

// Flies a body-relative slot on a MOVING lead.
//
// This is the capability the wingman needs and the codebase did not have. `escort` (the
// StateMachineController template in AiControllerFactory.h) captures its orbit centre ONCE at
// construction, so it escorts the place the lead was standing when the order was given — its own
// comment says a controller that tracks a moving lead is missing. This is that controller. It does
// not orbit: it holds station in the lead's frame, recomputing the slot point every tick.
//
// Everything is expressed in the LOCAL tangent frame at the lead's position (LocalFrame.h /
// Guidance.h), so it is correct anywhere on the sphere, not only near the world origin (#478).
//
// The part no existing controller has is CLOSED-LOOP THROTTLE. Every other controller here flies at
// a fixed throttle, which is fine for a pursuit: you either catch the target or you do not. A
// formation member must *hold station*, which is a range-rate problem — a fixed throttle overshoots a
// slow lead and never catches a fast one. So throttle is driven by along-track error and closure rate.
//
// Neutral output (no throttle, no surfaces) when the lead is dead or invalid, matching
// PursuitController's contract — a member whose lead is gone coasts rather than flying at a ghost.
class FormationController : public fl::IEntityController {
  public:
    FormationController(const fl::EntityManager& entityManager, fl::EntityId leadId, uint32_t slotIndex,
                        FormationParams params = {});

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::SpatialIndex* si = nullptr) override;

    [[nodiscard]] fl::EntityId lead() const noexcept {
        return m_leadId;
    }
    [[nodiscard]] uint32_t slotIndex() const noexcept {
        return m_slotIndex;
    }

    // World position of the slot this controller is flying to, for the lead's current transform.
    // Exposed for tests and for a future formation debug overlay.
    [[nodiscard]] glm::dvec3 slotPoint(const fl::EntityState& lead) const noexcept;

  private:
    const fl::EntityManager& m_entityManager;
    fl::EntityId m_leadId;
    uint32_t m_slotIndex;
    FormationParams m_params;
};

} // namespace fl::ai
