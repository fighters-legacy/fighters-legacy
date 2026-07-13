// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/FormationController.h"
#include "ai/WingmanCommand.h"
#include "entity/EntityId.h"
#include "entity/IEntityController.h"

#include <glm/glm.hpp>
#include <memory>

namespace fl {
class EntityManager;
}

namespace fl::ai {

// Tuning for the scripted flight behaviors. Defaults are deliberately conservative: a wingman that
// wanders off to fight something 30 km away is not a wingman.
struct WingmanParams {
    FormationParams formation{};
    uint32_t slotIndex{0}; // station in the formation (see formationSlotOffset)

    float engageRangeM{12000.f};        // engage_bandits: hostile within this of SELF triggers the attack
    float coverRangeM{6000.f};          // cover_me: hostile within this of the ANCHOR triggers the attack
    float breakoffRangeMult{1.5f};      // hysteresis: return to formation only outside range * this
    float attackAbandonRangeM{30000.f}; // attack_my_target: give up and rejoin past this
    float retreatHpFraction{0.25f};     // below this HP fraction, disengage and evade

    glm::dvec3 homePoint{0.0, 600.0, 0.0}; // return_to_base: where home is (the member's spawn point)
    float homeOrbitRadiusM{3000.f};
    float homeThrottle{0.8f};
};

// Build the controller that implements `cmd` for an AI flight member.
//
// `anchorId` is the entity the member forms on (a player, or an AI flight lead). `designatedTarget`
// is used ONLY by AttackMyTarget and may be invalid — the caller must have already refused the order
// with WingmanResult::NoTarget in that case, because an attack order that quietly picks its own
// target is worse than one that declines.
//
// Never returns nullptr for a valid command: every one of the six maps to a real controller today.
// (hold_fire has no weapons to hold until #583, so its flight behavior is "break off and hold
// station" — the weapons-hold flag itself lives on the FormationMember, where the firing trigger will
// read it. It is a real order with a real effect, not a no-op.)
[[nodiscard]] std::unique_ptr<fl::IEntityController> makeWingmanController(const fl::EntityManager& entityManager,
                                                                           fl::EntityId anchorId, WingmanCommand cmd,
                                                                           fl::EntityId designatedTarget,
                                                                           const WingmanParams& params);

} // namespace fl::ai
