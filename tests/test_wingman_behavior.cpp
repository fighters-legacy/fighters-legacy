// SPDX-License-Identifier: GPL-3.0-or-later
//
// FormationController, boresight designation, and the six scripted commands (#610).
//
// The load-bearing cases here are the ones that separate this from what already existed:
//   * FormationController tracks a MOVING lead (the gap `escort` names and does not fill).
//   * cover_me triggers on a threat near the LEAD, not near the wingman.
//   * attack_my_target REFUSES rather than inventing a target.
#include "ILogger.h"
#include "ai/FormationController.h"
#include "ai/StateMachineController.h"
#include "ai/Threat.h"
#include "ai/WingmanBehavior.h"
#include "ai/WingmanCommand.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "spatial/SpatialIndex.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <glm/glm.hpp>
#include <numbers>

using namespace fl;
using fl::ai::WingmanCommand;

namespace {

struct NullLogger final : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

EntityDef makeDef() {
    EntityDef d{};
    d.id = "test:basic";
    d.maxHp = 100.f;
    return d;
}

// Faction 1 = "our side"; faction 2 = hostile. areFactionsHostile: different non-zero factions.
constexpr uint16_t kOurs = 1;
constexpr uint16_t kEnemy = 2;

EntityId spawnAt(EntityManager& em, double x, double y, double z, uint16_t faction) {
    EntityTransform t{};
    t.pos[0] = x;
    t.pos[1] = y;
    t.pos[2] = z;
    t.quat[3] = 1.f; // identity: forward = +X
    const EntityId id = em.spawn("test:basic", t);
    if (EntityState* s = em.get(id))
        s->factionIndex = faction;
    return id;
}

// The spatial index the conditions query. Rebuilt from the live entity set, as WorldBroadcaster does
// each tick.
void rebuildIndex(SpatialIndex& si, const EntityManager& em) {
    si.clear();
    em.forEach([&](const EntityState& s) {
        if (!s.dead)
            si.insert(s.id.index, s.transform.pos);
    });
}

} // namespace

// ---------------------------------------------------------------------------
// FormationController — the moving-lead capability
// ---------------------------------------------------------------------------

TEST_CASE("formationSlotOffset alternates right/left and steps out each rank") {
    fl::ai::FormationParams p{};
    p.lateralM = 100.f;
    p.aftM = 50.f;
    p.verticalM = -10.f;

    const glm::vec3 s0 = fl::ai::formationSlotOffset(0, p);
    const glm::vec3 s1 = fl::ai::formationSlotOffset(1, p);
    const glm::vec3 s2 = fl::ai::formationSlotOffset(2, p);

    CHECK(s0.x > 0.f); // slot 0 is right
    CHECK(s1.x < 0.f); // slot 1 is left
    CHECK(s0.y < 0.f); // both are astern of the lead
    CHECK(s1.y < 0.f);
    CHECK(s0.z < 0.f); // stepped down

    // Rank 2 steps further out and further back — so a flight of ANY size has a defined station,
    // rather than running out of a fixed table of named positions.
    CHECK(std::abs(s2.x) > std::abs(s0.x));
    CHECK(std::abs(s2.y) > std::abs(s0.y));
}

TEST_CASE("FormationController flies at the slot on a MOVING lead, not where the lead used to be") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    const EntityId wingId = spawnAt(em, 0, 600, 0, kOurs);

    fl::ai::FormationController ctrl(em, leadId, /*slotIndex=*/0);

    const EntityState* lead = em.get(leadId);
    REQUIRE(lead != nullptr);
    const glm::dvec3 slotBefore = ctrl.slotPoint(*lead);

    // Move the lead 5 km downrange. This is exactly what `escort` cannot cope with: it captured its
    // orbit centre at construction, so it would still be circling the old spot.
    EntityState* mutableLead = em.get(leadId);
    mutableLead->transform.pos[0] = 5000.0;

    const glm::dvec3 slotAfter = ctrl.slotPoint(*mutableLead);
    CHECK(glm::length(slotAfter - slotBefore) > 4000.0); // the station moved with the lead

    // And the slot stays near the lead, not near the origin.
    const glm::dvec3 leadPos(mutableLead->transform.pos[0], mutableLead->transform.pos[1],
                             mutableLead->transform.pos[2]);
    CHECK(glm::length(slotAfter - leadPos) < 1000.0);

    (void)wingId;
}

TEST_CASE("FormationController opens the throttle when astern of its slot") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    // The wingman is 3 km BEHIND (lead faces +X, so behind is -X): it must accelerate to close.
    const EntityId wingId = spawnAt(em, -3000, 600, 0, kOurs);

    fl::ai::FormationParams p{};
    fl::ai::FormationController ctrl(em, leadId, 0, p);

    const EntityState* wing = em.get(wingId);
    REQUIRE(wing != nullptr);
    const ControlInput inp = ctrl.sample(*wing, 0, 1.0 / 60.0);

    // Closed-loop throttle: above base because there is along-track error to close. A fixed-throttle
    // pursuit could never hold station on a lead whose speed varies.
    CHECK(inp.throttle > p.throttleBase);
    CHECK(inp.afterburner); // a rejoin from 3 km out is a stern chase
}

TEST_CASE("FormationController coasts when the lead is dead") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    const EntityId wingId = spawnAt(em, 100, 600, 0, kOurs);

    fl::ai::FormationController ctrl(em, leadId, 0);
    em.kill(leadId);

    const EntityState* wing = em.get(wingId);
    REQUIRE(wing != nullptr);
    const ControlInput inp = ctrl.sample(*wing, 0, 1.0 / 60.0);

    CHECK(inp.throttle == 0.f); // no flying at a ghost
    CHECK(inp.aileron == 0.f);
    CHECK(inp.elevator == 0.f);
}

// ---------------------------------------------------------------------------
// Boresight designation — the provisional stand-in for a radar lock
// ---------------------------------------------------------------------------

TEST_CASE("designateBoresightTarget picks the hostile nearest the look axis") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    const EntityId ahead = spawnAt(em, 5000, 600, 0, kEnemy);      // dead ahead
    const EntityId offAxis = spawnAt(em, 1000, 600, 4000, kEnemy); // closer, but well off-axis

    const EntityState* lead = em.get(leadId);
    REQUIRE(lead != nullptr);

    const float viewAxis[3] = {1.f, 0.f, 0.f}; // looking down +X
    const EntityId got =
        fl::ai::designateBoresightTarget(em, *lead, viewAxis, 15000.f, 15.f * std::numbers::pi_v<float> / 180.f);

    // "My target" is what the lead is LOOKING at — a nearer bandit off to the side is not it.
    CHECK(got == ahead);
    CHECK_FALSE(got == offAxis);
}

TEST_CASE("designateBoresightTarget ignores friendlies and neutrals") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    spawnAt(em, 3000, 600, 0, kOurs); // a friendly, dead ahead
    spawnAt(em, 4000, 600, 0, 0);     // a neutral, dead ahead

    const EntityState* lead = em.get(leadId);
    const float viewAxis[3] = {1.f, 0.f, 0.f};
    const EntityId got =
        fl::ai::designateBoresightTarget(em, *lead, viewAxis, 15000.f, 15.f * std::numbers::pi_v<float> / 180.f);

    CHECK_FALSE(got.valid()); // refusing beats fratricide
}

TEST_CASE("designateBoresightTarget refuses when nothing is in the cone") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    spawnAt(em, -5000, 600, 0, kEnemy); // BEHIND the lead
    spawnAt(em, 50000, 600, 0, kEnemy); // ahead but far outside range

    const EntityState* lead = em.get(leadId);
    const float viewAxis[3] = {1.f, 0.f, 0.f};
    const EntityId got =
        fl::ai::designateBoresightTarget(em, *lead, viewAxis, 15000.f, 15.f * std::numbers::pi_v<float> / 180.f);

    // No target must mean NO TARGET. An attack order that quietly picks its own is worse than one
    // that declines — the server turns this into WingmanResult::NoTarget ("Two, no joy").
    CHECK_FALSE(got.valid());
}

TEST_CASE("a neutral lead can designate nothing") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    // faction 0 = neutral = no enemies. This is exactly why players must carry a faction.
    const EntityId leadId = spawnAt(em, 0, 600, 0, /*faction=*/0);
    spawnAt(em, 3000, 600, 0, kEnemy);

    const EntityState* lead = em.get(leadId);
    const float viewAxis[3] = {1.f, 0.f, 0.f};
    CHECK_FALSE(fl::ai::designateBoresightTarget(em, *lead, viewAxis, 15000.f, 15.f * std::numbers::pi_v<float> / 180.f)
                    .valid());
}

// ---------------------------------------------------------------------------
// The six commands
// ---------------------------------------------------------------------------

TEST_CASE("makeWingmanController builds a controller for every command in the grammar") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    const EntityId targetId = spawnAt(em, 5000, 600, 0, kEnemy);

    fl::ai::WingmanParams p{};
    for (uint8_t i = 0; i < fl::ai::kWingmanCommandCount; ++i) {
        const auto cmd = static_cast<WingmanCommand>(i);
        auto ctrl = fl::ai::makeWingmanController(em, leadId, cmd, targetId, p);
        INFO("command: " << fl::ai::wingmanCommandName(cmd));
        CHECK(ctrl != nullptr); // all six are real behaviors today, including hold_fire
    }
}

TEST_CASE("cover_me engages a threat closing on the LEAD, not one near the wingman") {
    // This is the single case that distinguishes cover_me from engage_bandits, and the reason
    // AnyHostileEntityWithinRangeOf had to exist: geometry about the anchor, hostility about self.
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);
    SpatialIndex si;

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    // The wingman is far from the lead, so a threat near the LEAD is nowhere near the wingman.
    const EntityId wingId = spawnAt(em, 20000, 600, 0, kOurs);

    fl::ai::WingmanParams p{};
    p.coverRangeM = 6000.f;
    auto ctrl = fl::ai::makeWingmanController(em, leadId, WingmanCommand::CoverMe, EntityId{}, p);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    const EntityState* wing = em.get(wingId);
    REQUIRE(wing != nullptr);

    rebuildIndex(si, em);
    sm->sample(*wing, 0, 1.0 / 60.0, fl::AiTickContext{&si});
    CHECK(sm->currentState() == "form"); // sky is clear

    // Put a bandit 2 km from the LEAD (and ~18 km from the wingman).
    spawnAt(em, 2000, 600, 0, kEnemy);
    rebuildIndex(si, em);

    // The transition carries 1 s of dwell (hysteresis, so a threat skimming the range boundary does
    // not make the wingman flip-flop), so it needs more than a tick or two of simulated time.
    for (int i = 0; i < 120; ++i)
        sm->sample(*wing, static_cast<uint64_t>(i + 1), 1.0 / 60.0, fl::AiTickContext{&si});
    CHECK(sm->currentState() == "engage"); // it went to protect the LEAD
}

TEST_CASE("engage_bandits ignores a threat that is only near the lead") {
    // The mirror image of the case above: engage_bandits is anchored on SELF, so a bandit closing on
    // the lead far away is not its business — that is what cover_me is for.
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);
    SpatialIndex si;

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    const EntityId wingId = spawnAt(em, 50000, 600, 0, kOurs); // far from the lead

    fl::ai::WingmanParams p{};
    p.engageRangeM = 12000.f;
    auto ctrl = fl::ai::makeWingmanController(em, leadId, WingmanCommand::EngageBandits, EntityId{}, p);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    spawnAt(em, 1000, 600, 0, kEnemy); // right next to the LEAD, 49 km from the wingman
    rebuildIndex(si, em);

    const EntityState* wing = em.get(wingId);
    // Well past the dwell the cover_me case needs — so this is a real "never fires", not "not yet".
    for (int i = 0; i < 120; ++i)
        sm->sample(*wing, static_cast<uint64_t>(i), 1.0 / 60.0, fl::AiTickContext{&si});
    CHECK(sm->currentState() == "form"); // stays on the wing
}

TEST_CASE("attack_my_target holds station rather than inventing a target when none was designated") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);

    // An invalid designated target reaches here only if the caller ignored the NoTarget refusal. Even
    // then the controller must not go hunting on its own.
    auto ctrl = fl::ai::makeWingmanController(em, leadId, WingmanCommand::AttackMyTarget, EntityId{}, {});
    REQUIRE(ctrl != nullptr);
    CHECK(dynamic_cast<fl::ai::FormationController*>(ctrl.get()) != nullptr);
}

TEST_CASE("attack_my_target rejoins the formation once the target is dead") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);
    SpatialIndex si;

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);
    const EntityId wingId = spawnAt(em, 100, 600, 0, kOurs);
    const EntityId targetId = spawnAt(em, 3000, 600, 0, kEnemy);

    auto ctrl = fl::ai::makeWingmanController(em, leadId, WingmanCommand::AttackMyTarget, targetId, {});
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    const EntityState* wing = em.get(wingId);
    rebuildIndex(si, em);
    sm->sample(*wing, 0, 1.0 / 60.0, fl::AiTickContext{&si});
    CHECK(sm->currentState() == "attack");

    // Kill the bandit. ThreatBeyondRange is true for a dead/invalid target, so the wingman rejoins
    // with no kill-detection code of its own. It needs 2 s of dwell first.
    em.kill(targetId);
    rebuildIndex(si, em);
    for (int i = 0; i < 200; ++i)
        sm->sample(*wing, static_cast<uint64_t>(i + 1), 1.0 / 60.0, fl::AiTickContext{&si});

    CHECK(sm->currentState() == "form");
}

TEST_CASE("rejoin and hold_fire both fly formation") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId leadId = spawnAt(em, 0, 600, 0, kOurs);

    // hold_fire has no weapons to hold until #583; its flight profile is "break off and hold
    // station", and the weapons-hold FLAG lives on the FormationMember where the firing trigger will
    // read it. It is a real order with a real effect, not a no-op.
    for (const auto cmd : {WingmanCommand::Rejoin, WingmanCommand::HoldFire}) {
        auto ctrl = fl::ai::makeWingmanController(em, leadId, cmd, EntityId{}, {});
        REQUIRE(ctrl != nullptr);
        CHECK(dynamic_cast<fl::ai::FormationController*>(ctrl.get()) != nullptr);
    }
}

TEST_CASE("nearestHostileWithin picks the closest enemy and ignores its own side") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef());
    EntityManager em(log, reg);

    const EntityId anchorId = spawnAt(em, 0, 600, 0, kOurs);
    spawnAt(em, 500, 600, 0, kOurs); // a friendly, very close
    const EntityId near = spawnAt(em, 2000, 600, 0, kEnemy);
    spawnAt(em, 8000, 600, 0, kEnemy); // a further enemy

    const EntityState* anchor = em.get(anchorId);
    REQUIRE(anchor != nullptr);
    const EntityId got = fl::ai::nearestHostileWithin(em, *anchor, kOurs, 12000.f);
    CHECK(got == near);
}
