// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/WingmanBehavior.h"

#include "ai/EvadeController.h"
#include "ai/LeadPursuitController.h"
#include "ai/LoiterController.h"
#include "ai/StateMachineController.h"
#include "ai/Threat.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"

namespace fl::ai {
namespace {

// Formation state: hold station on the anchor. Shared by every command that returns to formation.
ControllerFactory formationFactory(const fl::EntityManager& em, fl::EntityId anchorId, const WingmanParams& p) {
    return [&em, anchorId, p]() -> std::unique_ptr<fl::IEntityController> {
        return std::make_unique<FormationController>(em, anchorId, p.slotIndex, p.formation);
    };
}

// Engage state: pick the nearest hostile to `anchor` AT STATE ENTRY and attack it.
//
// The threat is resolved inside the factory rather than captured at construction, and that is the
// whole point: StateMachineController rebuilds the child controller every time the state is entered,
// so re-entering `engage` finds a FRESH bandit. A threat captured once would have the wingman
// chasing a corpse after its first kill.
ControllerFactory engageFactory(const fl::EntityManager& em, fl::EntityId anchorId, float rangeM,
                                const WingmanParams& p) {
    return [&em, anchorId, rangeM, p]() -> std::unique_ptr<fl::IEntityController> {
        // Anchor the threat search on whichever entity we are protecting; for engage_bandits the
        // caller passes the member's own anchor, for cover_me it is the lead being covered.
        const fl::EntityState* anchor = em.get(anchorId);
        if (!anchor) {
            return std::make_unique<FormationController>(em, anchorId, p.slotIndex, p.formation);
        }
        // Hostility is judged against the anchor's faction: a flight member shares its lead's side.
        const fl::EntityId threat = nearestHostileWithin(em, *anchor, anchor->factionIndex, rangeM);
        if (!threat.valid()) {
            // Nothing to hit — hold station rather than flying at nothing.
            return std::make_unique<FormationController>(em, anchorId, p.slotIndex, p.formation);
        }
        return std::make_unique<LeadPursuitController>(em, threat, 1.0f, 0.9f, /*useAfterburner=*/true);
    };
}

} // namespace

std::unique_ptr<fl::IEntityController> makeWingmanController(const fl::EntityManager& em, fl::EntityId anchorId,
                                                             WingmanCommand cmd, fl::EntityId designatedTarget,
                                                             const WingmanParams& params) {
    switch (cmd) {
    case WingmanCommand::Rejoin:
    case WingmanCommand::HoldFire:
        // Both fly the same profile: back to station on the anchor. What separates them is the
        // weapons-hold flag on the FormationMember, which an engage order clears (#583 gives it teeth
        // when there is a trigger to gate).
        return std::make_unique<FormationController>(em, anchorId, params.slotIndex, params.formation);

    case WingmanCommand::ReturnToBase: {
        // LoiterController already flies TO its circle from anywhere and then orbits it, which is
        // exactly "disengage and hold over home". Landing needs a landing system; this is honest.
        const auto altM = static_cast<float>(params.homePoint.y);
        return std::make_unique<LoiterController>(params.homePoint, params.homeOrbitRadiusM, altM, params.homeThrottle,
                                                  LoiterDir::Clockwise);
    }

    case WingmanCommand::AttackMyTarget: {
        if (!designatedTarget.valid()) {
            // The caller should have refused with NoTarget. Hold station rather than inventing one.
            return std::make_unique<FormationController>(em, anchorId, params.slotIndex, params.formation);
        }
        auto sm = std::make_unique<StateMachineController>(em);
        const WingmanParams p = params;
        sm->addState("attack", [&em, designatedTarget]() -> std::unique_ptr<fl::IEntityController> {
            return std::make_unique<LeadPursuitController>(em, designatedTarget, 1.0f, 0.9f, /*useAfterburner=*/true);
        });
        sm->addState("form", formationFactory(em, anchorId, p));
        sm->addState("evade", [&em, designatedTarget]() -> std::unique_ptr<fl::IEntityController> {
            return std::make_unique<EvadeController>(em, designatedTarget);
        });
        // ThreatBeyondRange is true when the target is dead, invalid, OR far — so this single
        // transition gives us "rejoin after the kill" and "give up on a bandit that ran away" for
        // free, with no kill-detection code.
        sm->addTransition("attack", "form", ThreatBeyondRange(designatedTarget, p.attackAbandonRangeM), 2.f);
        sm->addTransition("attack", "evade", HpBelow(p.retreatHpFraction), 1.f);
        sm->addTransition("evade", "form", ThreatBeyondRange(designatedTarget, p.attackAbandonRangeM), 4.f);
        sm->setInitialState("attack");
        return sm;
    }

    case WingmanCommand::EngageBandits: {
        auto sm = std::make_unique<StateMachineController>(em);
        const WingmanParams p = params;
        const float breakoff = p.engageRangeM * p.breakoffRangeMult;
        sm->addState("form", formationFactory(em, anchorId, p));
        sm->addState("engage", engageFactory(em, anchorId, p.engageRangeM, p));
        sm->addState("evade", [&em, anchorId]() -> std::unique_ptr<fl::IEntityController> {
            return std::make_unique<EvadeController>(em, anchorId); // break away from the fight
        });
        sm->addTransition("form", "engage", AnyHostileEntityWithinRange(p.engageRangeM), 1.f);
        sm->addTransition("engage", "evade", HpBelow(p.retreatHpFraction), 1.f);
        // Hysteresis: only give up the fight once the sky is clear well beyond the trigger ring,
        // otherwise the wingman oscillates on the boundary.
        sm->addTransition("engage", "form", Not(AnyHostileEntityWithinRange(breakoff)), 5.f);
        sm->addTransition("evade", "form", Not(AnyHostileEntityWithinRange(breakoff)), 8.f);
        sm->setInitialState("form");
        return sm;
    }

    case WingmanCommand::CoverMe: {
        // Same shape as engage_bandits, but every threat question is asked about the ANCHOR, not
        // about the wingman. That one substitution is the whole difference between "fight whatever
        // finds you" and "protect the lead".
        auto sm = std::make_unique<StateMachineController>(em);
        const WingmanParams p = params;
        const float breakoff = p.coverRangeM * p.breakoffRangeMult;
        sm->addState("form", formationFactory(em, anchorId, p));
        sm->addState("engage", engageFactory(em, anchorId, p.coverRangeM, p));
        sm->addTransition("form", "engage", AnyHostileEntityWithinRangeOf(anchorId, p.coverRangeM), 1.f);
        sm->addTransition("engage", "form", Not(AnyHostileEntityWithinRangeOf(anchorId, breakoff)), 8.f);
        sm->setInitialState("form");
        return sm;
    }

    case WingmanCommand::Count:
    default:
        return nullptr;
    }
}

} // namespace fl::ai
