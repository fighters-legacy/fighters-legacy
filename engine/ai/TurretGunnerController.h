// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityManager.h"
#include "entity/ISeatController.h"

#include <cstdint>

namespace fl::ai {

// ── Bot turret gunner (#966/#971) ────────────────────────────────────────────
//
// The default bot for a defensive turret seat: it acquires HONESTLY off the airframe's shared contact
// table (a jammed/blind aircraft's gunner engages nothing), leads the target with the shared
// BallisticLead solver, commands the turret onto the lead point (the server servo slews + clamps it),
// and holds the trigger only once the turret is actually pointed at the target and the predicted miss
// is inside the lethal cone — the AaaFireController discipline, but as an ISeatController producing a
// SeatCommand instead of steering an airframe.
//
// PER-INSTANCE SKILL (the first in the engine). Rolled once from a deterministic seed (mission seed XOR
// object id XOR seat index) into [skillMin, skillMax], then it drives TWO things nothing consumed
// before: an aim-error jitter scaled off AiScaling::aimErrorDeg (its first consumer), and a reaction
// delay before the gunner opens fire. A higher rolled skill measurably tightens the aim and shortens
// the reaction. Deterministic — a replay and a 1-vs-N-worker run are byte-identical.
class TurretGunnerController final : public ISeatController {
  public:
    TurretGunnerController(const EntityManager& entityManager, float skillMin, float skillMax,
                           float engageRangeM = 1500.f, float muzzleVelMps = 1000.f, float lethalRadiusM = 12.f,
                           uint64_t missionSeed = 0);

    SeatCommand sample(const EntityState& airframe, const SeatView& seat, uint64_t tick, double dt,
                       const AiTickContext& ctx) override;

    // The skill this instance rolled (valid after the first sample). For tests / telemetry.
    [[nodiscard]] float rolledSkill() const noexcept {
        return m_skill;
    }

  private:
    const EntityManager& m_entityManager;
    float m_skillMin;
    float m_skillMax;
    float m_engageRangeM;
    float m_muzzleVelMps;
    float m_lethalRadiusM;
    uint64_t m_missionSeed;

    bool m_skillRolled{false};
    float m_skill{0.5f};
    uint64_t m_engageStartTick{0}; // first tick of the current continuous engagement (reaction timer)
    bool m_engaged{false};
};

} // namespace fl::ai
