// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/AiControllerFactory.h"

#include "ai/BallisticGuidanceController.h"
#include "ai/BreakTurnController.h"
#include "ai/DynamicLoiterController.h"
#include "ai/EvadeController.h"
#include "ai/FormationController.h"
#include "ai/GunsEmploymentController.h"
#include "ai/HighYoYoController.h"
#include "ai/ImmelmannController.h"
#include "ai/LagPursuitController.h"
#include "ai/LeadPursuitController.h"
#include "ai/LoiterController.h"
#include "ai/LowYoYoController.h"
#include "ai/PursuitController.h"
#include "ai/SplitSController.h"
#include "ai/StateMachineController.h"   // exposes Condition helpers + StateMachineController
#include "ai/SurfaceThreatControllers.h" // sam / aaa emplacements (#863)
#include "ai/SwarmController.h"          // boids swarm member (#353)
#include "ai/WaypointController.h"
#include "ai/WingmanBehavior.h" // makeWingmanController + WingmanParams
#include "ai/WingmanCommand.h"  // the six-command grammar
#include "entity/EntityManager.h"
#include "util/Parse.h" // the ONE strict number parser (#1244)

#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace fl::ai {

namespace {

// A positional-argument reader for the behavior grammar (#1265).
//
// Twenty-odd clauses each hand-wrote the same four shapes: "argument N must name a live entity",
// "argument N is an optional number, and a malformed one is an error rather than a default",
// "...and it must be positive", "argument N is cw or ccw". Written out longhand that is six lines
// per optional argument, which is how this file reached 700 inline lines in a header.
//
// The reader carries a STICKY failure flag rather than returning a bool per read. That is what lets
// a clause read its whole argument list and check once, and it is also the safer default: a missed
// check leaves the flag set, so the spawn fails, instead of silently constructing a controller from
// a half-parsed argument list.
//
// Absent optional arguments leave their destination untouched (the caller's default stands).
// PRESENT ones are validated and REJECTED on failure -- never quietly defaulted. An operator who
// typed a number meant it, and substituting a default is how a mission flies the wrong loiter
// radius with nothing in the log to say so.
class ArgCursor {
  public:
    ArgCursor(std::span<std::string_view> args, const fl::EntityManager* em) noexcept : m_args(args), m_em(em) {}

    [[nodiscard]] bool ok() const noexcept {
        return m_ok;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return m_args.size();
    }
    [[nodiscard]] bool has(std::size_t i) const noexcept {
        return i < m_args.size();
    }
    [[nodiscard]] std::string_view at(std::size_t i) const noexcept {
        return m_args[i];
    }
    void reject() noexcept {
        m_ok = false;
    }

    // A LIVE entity named by pool index at args[i]. Fails when the argument is missing or malformed,
    // when there is no entity manager, or when no live entity carries that index.
    //
    // Must be called on the sim thread (EntityManager::forEach is sim-thread only).
    [[nodiscard]] fl::EntityId requiredEntity(std::size_t i) {
        uint32_t idx = 0;
        if (!m_em || !has(i) || !fl::readInto(fl::parseU32(m_args[i]), idx)) {
            m_ok = false;
            return fl::EntityId::null();
        }
        fl::EntityId found;
        m_em->forEach([&](const fl::EntityState& s) {
            if (!found.valid() && !s.dead && s.id.index == idx)
                found = s.id;
        });
        if (!found.valid())
            m_ok = false;
        return found;
    }

    // A positional double that must be present.
    [[nodiscard]] double requiredDouble(std::size_t i) {
        double d = 0.0;
        if (!has(i) || !fl::readInto(fl::parseDouble(m_args[i]), d))
            m_ok = false;
        return d;
    }

    // Optional double, bounded to the INCLUSIVE range [lo, hi].
    void optDouble(std::size_t i, double& out, double lo = -kInf, double hi = kInf) {
        if (const auto d = readBounded(i, lo, hi))
            out = *d;
    }

    void optFloat(std::size_t i, float& out, double lo = -kInf, double hi = kInf) {
        if (const auto d = readBounded(i, lo, hi))
            out = static_cast<float>(*d);
    }

    // Strictly greater than zero -- deliberately NOT optFloat(i, out, 0.0, kInf), which admits zero.
    // A zero muzzle velocity or engagement range is not a slow weapon, it is a broken spawn, and
    // every site that used this spelled it `d <= 0.0` for that reason.
    void optPositiveFloat(std::size_t i, float& out) {
        if (!has(i))
            return;
        double d = 0.0;
        if (!fl::readInto(fl::parseDouble(m_args[i]), d) || d <= 0.0) {
            m_ok = false;
            return;
        }
        out = static_cast<float>(d);
    }

    void optU32(std::size_t i, uint32_t& out, uint32_t hi = std::numeric_limits<uint32_t>::max()) {
        uint32_t v = 0;
        if (!has(i))
            return;
        if (!fl::readInto(fl::parseU32(m_args[i]), v) || v > hi) {
            m_ok = false;
            return;
        }
        out = v;
    }

    void optDir(std::size_t i, LoiterDir& out) {
        if (!has(i))
            return;
        if (m_args[i] == "ccw")
            out = LoiterDir::CounterClockwise;
        else if (m_args[i] == "cw")
            out = LoiterDir::Clockwise;
        else
            m_ok = false;
    }

  private:
    static constexpr double kInf = std::numeric_limits<double>::infinity();

    // Absent -> nullopt, caller's default stands. Present and in range -> the value. Present and
    // malformed or out of range -> nullopt AND the sticky flag drops, which is the difference
    // between "you did not say" and "what you said is not a number".
    [[nodiscard]] std::optional<double> readBounded(std::size_t i, double lo, double hi) {
        if (!has(i))
            return std::nullopt;
        double d = 0.0;
        if (!fl::readInto(fl::parseDouble(m_args[i]), d) || d < lo || d > hi) {
            m_ok = false;
            return std::nullopt;
        }
        return d;
    }

    std::span<std::string_view> m_args;
    const fl::EntityManager* m_em{nullptr};
    bool m_ok{true};
};

} // namespace

std::unique_ptr<fl::IEntityController> createController(std::string_view behavior, std::span<std::string_view> args,
                                                        const fl::EntityManager* entityManager) {
    ArgCursor c(args, entityManager);

    // -----------------------------------------------------------------------
    // loiter [cx cy cz [radius_m [alt_m [throttle [cw|ccw]]]]]
    // -----------------------------------------------------------------------
    if (behavior == "loiter") {
        glm::dvec3 center{0.0, 600.0, 0.0};
        float radius = 3000.f;
        float alt = 600.f;
        float thr = 0.65f;
        LoiterDir dir = LoiterDir::Clockwise;

        if (c.size() >= 3) {
            center.x = c.requiredDouble(0);
            center.y = c.requiredDouble(1);
            center.z = c.requiredDouble(2);
        }
        c.optFloat(3, radius);
        c.optFloat(4, alt);
        c.optFloat(5, thr);
        c.optDir(6, dir);
        if (!c.ok())
            return nullptr;
        return std::make_unique<LoiterController>(center, radius, alt, thr, dir);
    }

    // -----------------------------------------------------------------------
    // dynamic_loiter  <entityIdx> [radius_m [throttle [cw|ccw]]]
    // Orbits a MOVING entity (re-centers each tick), unlike the fixed-center loiter above.
    // -----------------------------------------------------------------------
    if (behavior == "dynamic_loiter") {
        const fl::EntityId id = c.requiredEntity(0);
        float radius = 3000.f;
        float thr = 0.65f;
        LoiterDir dir = LoiterDir::Clockwise;
        c.optFloat(1, radius);
        c.optFloat(2, thr);
        c.optDir(3, dir);
        if (!c.ok())
            return nullptr;
        return std::make_unique<DynamicLoiterController>(*entityManager, id, radius, thr, dir);
    }

    // -----------------------------------------------------------------------
    // waypoint  x1 y1 z1 [x2 y2 z2 ...] [--loop]
    // -----------------------------------------------------------------------
    if (behavior == "waypoint") {
        bool loop = false;
        std::vector<std::string_view> coordArgs;
        coordArgs.reserve(args.size());
        for (auto& a : args) {
            if (a == "--loop")
                loop = true;
            else
                coordArgs.push_back(a);
        }
        if (coordArgs.empty() || coordArgs.size() % 3 != 0)
            return nullptr;

        // The --loop filter reshuffles the positions, so the waypoint list gets its own cursor over
        // the coordinates that survived it.
        ArgCursor wc(coordArgs, entityManager);
        std::vector<glm::dvec3> wps;
        wps.reserve(coordArgs.size() / 3);
        for (std::size_t i = 0; i < coordArgs.size(); i += 3)
            wps.push_back({wc.requiredDouble(i), wc.requiredDouble(i + 1), wc.requiredDouble(i + 2)});
        if (!wc.ok())
            return nullptr;
        return std::make_unique<WaypointController>(std::move(wps), 500.f, 0.7f, loop);
    }

    // -----------------------------------------------------------------------
    // pursuit  <entityIdx>
    // -----------------------------------------------------------------------
    if (behavior == "pursuit") {
        const fl::EntityId id = c.requiredEntity(0);
        if (!c.ok())
            return nullptr;
        return std::make_unique<PursuitController>(*entityManager, id);
    }

    // -----------------------------------------------------------------------
    // evade  <entityIdx>
    // -----------------------------------------------------------------------
    if (behavior == "evade") {
        const fl::EntityId id = c.requiredEntity(0);
        if (!c.ok())
            return nullptr;
        return std::make_unique<EvadeController>(*entityManager, id);
    }

    // -----------------------------------------------------------------------
    // break  <entityIdx> [rollDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "break") {
        const fl::EntityId id = c.requiredEntity(0);
        float rollDur = 0.5f;
        c.optFloat(1, rollDur);
        if (!c.ok())
            return nullptr;
        return std::make_unique<BreakTurnController>(*entityManager, id, rollDur);
    }

    // -----------------------------------------------------------------------
    // lead  <entityIdx> [navGain]
    // -----------------------------------------------------------------------
    if (behavior == "lead") {
        const fl::EntityId id = c.requiredEntity(0);
        float navGain = 1.0f;
        c.optFloat(1, navGain);
        if (!c.ok())
            return nullptr;
        return std::make_unique<LeadPursuitController>(*entityManager, id, navGain);
    }

    // -----------------------------------------------------------------------
    // lag  <entityIdx> [lagFraction]
    // -----------------------------------------------------------------------
    if (behavior == "lag") {
        const fl::EntityId id = c.requiredEntity(0);
        float lagFraction = 1.0f;
        c.optFloat(1, lagFraction);
        if (!c.ok())
            return nullptr;
        return std::make_unique<LagPursuitController>(*entityManager, id, lagFraction);
    }

    // -----------------------------------------------------------------------
    // immelmann  [pullDurationS] [rollDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "immelmann") {
        float pullDur = 4.0f;
        float rollDur = 1.5f;
        c.optFloat(0, pullDur);
        c.optFloat(1, rollDur);
        if (!c.ok())
            return nullptr;
        return std::make_unique<ImmelmannController>(pullDur, rollDur);
    }

    // -----------------------------------------------------------------------
    // split_s  [rollDurationS] [pullDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "split_s") {
        float rollDur = 1.5f;
        float pullDur = 4.0f;
        c.optFloat(0, rollDur);
        c.optFloat(1, pullDur);
        if (!c.ok())
            return nullptr;
        return std::make_unique<SplitSController>(rollDur, pullDur);
    }

    // -----------------------------------------------------------------------
    // high_yo_yo  <entityIdx> [climbDurationS] [reacquireDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "high_yo_yo") {
        const fl::EntityId id = c.requiredEntity(0);
        float climbDur = 2.5f;
        float reacquireDur = 3.0f;
        c.optFloat(1, climbDur);
        c.optFloat(2, reacquireDur);
        if (!c.ok())
            return nullptr;
        return std::make_unique<HighYoYoController>(*entityManager, id, climbDur, reacquireDur);
    }

    // -----------------------------------------------------------------------
    // low_yo_yo  <entityIdx> [diveDurationS] [pullDurationS]
    // -----------------------------------------------------------------------
    if (behavior == "low_yo_yo") {
        const fl::EntityId id = c.requiredEntity(0);
        float diveDur = 1.5f;
        float pullDur = 2.5f;
        c.optFloat(1, diveDur);
        c.optFloat(2, pullDur);
        if (!c.ok())
            return nullptr;
        return std::make_unique<LowYoYoController>(*entityManager, id, diveDur, pullDur);
    }

    // -----------------------------------------------------------------------
    // ballistic  <tx> <ty> <tz> [mirvCount [spreadM]]  (#355)
    // -----------------------------------------------------------------------
    if (behavior == "ballistic") {
        if (args.size() < 3)
            return nullptr;
        BallisticGuidanceController::Params p;
        p.targetPos.x = c.requiredDouble(0);
        p.targetPos.y = c.requiredDouble(1);
        p.targetPos.z = c.requiredDouble(2);
        uint32_t mirv = static_cast<uint32_t>(p.mirvCount);
        c.optU32(3, mirv, /*hi=*/64);
        c.optDouble(4, p.mirvSpreadM, /*lo=*/0.0);
        if (!c.ok())
            return nullptr;
        p.mirvCount = static_cast<int>(mirv);
        return std::make_unique<BallisticGuidanceController>(p);
    }

    // -----------------------------------------------------------------------
    // guns  <entityIdx> [muzzleVelMps=1030] [lethalRadiusM=8]
    // -----------------------------------------------------------------------
    if (behavior == "guns") {
        const fl::EntityId id = c.requiredEntity(0);
        float muzzleVel = 1030.f;
        float lethalRadius = 8.f;
        c.optPositiveFloat(1, muzzleVel);
        c.optPositiveFloat(2, lethalRadius);
        if (!c.ok())
            return nullptr;
        return std::make_unique<GunsEmploymentController>(*entityManager, id, muzzleVel, lethalRadius);
    }

    // -----------------------------------------------------------------------
    // sam  [engageRangeM=30000] [coneHalfDeg=90] [fireIntervalS=4] [launchElevMinDeg=35]
    //      (#863 — static SAM launcher; auto-engages hostiles it detects on radar, no target index.
    //       launchElevMinDeg is the #1204 elevation floor on the launch vector: 0 fires flat, which
    //       on an emplacement standing on the deck means the store is reaped by the ground.)
    // -----------------------------------------------------------------------
    if (behavior == "sam") {
        if (!entityManager)
            return nullptr;
        float rangeM = 30000.f, coneHalfDeg = 90.f, fireIntervalS = 4.f, launchElevMinDeg = 35.f;
        c.optPositiveFloat(0, rangeM);
        c.optPositiveFloat(1, coneHalfDeg);
        c.optPositiveFloat(2, fireIntervalS);
        // 0 is legal here (a launcher that genuinely fires flat), unlike the three above.
        c.optFloat(3, launchElevMinDeg, /*lo=*/0.0, /*hi=*/89.0);
        if (!c.ok())
            return nullptr;
        return std::make_unique<SamEngagementController>(*entityManager, rangeM, coneHalfDeg, fireIntervalS,
                                                         launchElevMinDeg);
    }

    // -----------------------------------------------------------------------
    // aaa  [engageRangeM=4000] [coneHalfDeg=25] [muzzleVelMps=1000] [lethalRadiusM=15]   (#863 — static
    //      AAA gun; auto-leads and engages hostiles in its cone)
    // -----------------------------------------------------------------------
    if (behavior == "aaa") {
        if (!entityManager)
            return nullptr;
        float rangeM = 1200.f, coneHalfDeg = 25.f, muzzleVel = 1000.f, lethalRadius = 15.f;
        c.optPositiveFloat(0, rangeM);
        c.optPositiveFloat(1, coneHalfDeg);
        c.optPositiveFloat(2, muzzleVel);
        c.optPositiveFloat(3, lethalRadius);
        if (!c.ok())
            return nullptr;
        return std::make_unique<AaaFireController>(*entityManager, rangeM, coneHalfDeg, muzzleVel, lethalRadius);
    }

    // -----------------------------------------------------------------------
    // patrol_attack  <entityIdx> [engageRangeM=8000] [retreatHp=0.25]
    // -----------------------------------------------------------------------
    if (behavior == "patrol_attack") {
        const fl::EntityId id = c.requiredEntity(0);
        float engageRangeM = 8000.f;
        float retreatHp = 0.25f;
        c.optFloat(1, engageRangeM);
        c.optFloat(2, retreatHp);
        if (!c.ok())
            return nullptr;

        // Patrol loiter center: above the target's XZ position at 600 m altitude.
        // Captured at factory-creation time; fixed for the lifetime of this controller.
        const fl::EntityState* ts = entityManager->get(id);
        glm::dvec3 patrolCenter =
            ts ? glm::dvec3(ts->transform.pos[0], 600.0, ts->transform.pos[2]) : glm::dvec3{0.0, 600.0, 0.0};

        auto sm = std::make_unique<StateMachineController>(*entityManager);
        sm->addState("patrol", [patrolCenter]() {
            return std::make_unique<LoiterController>(patrolCenter, 3000.f, 600.f, 0.65f, LoiterDir::Clockwise);
        });
        sm->addState("engage", [entityManager, id]() {
            return std::make_unique<LeadPursuitController>(*entityManager, id, 1.0f, 0.9f, false);
        });
        sm->addState("retreat", [entityManager, id]() {
            return std::make_unique<EvadeController>(*entityManager, id, 1.0f, true);
        });
        // HONEST TRIGGERS (#690). The behavior string is unchanged, so `spawn --ai patrol_attack <idx>`
        // means exactly what it always did to an operator — but the AI now engages only what it has
        // actually DETECTED and REACTED to, and goes back to patrol when it has lost the contact
        // rather than when the target crosses an invisible radius it could never have measured. With
        // no sensing evaluated (unit tests, headless callers) these fall back to their ground-truth
        // ancestors, so nothing that worked before behaves differently.
        sm->addTransition("patrol", "engage", DetectsThreatWithinRange(id, engageRangeM));
        sm->addTransition("engage", "retreat", HpBelow(retreatHp));
        sm->addTransition("engage", "patrol", LostContact(id, engageRangeM * 1.5f), 2.f);
        sm->setInitialState("patrol");
        return sm;
    }

    // -----------------------------------------------------------------------
    // escort  <entityIdx> [standoffM=2000]
    // -----------------------------------------------------------------------
    if (behavior == "escort") {
        const fl::EntityId escortedId = c.requiredEntity(0);
        float standoffM = 2000.f;
        c.optFloat(1, standoffM);
        if (!c.ok())
            return nullptr;

        // The escort orbits the escortee at standoffM radius, tracking it as it MOVES (#464): the
        // follow state is a DynamicLoiterController re-centred each tick on the escortee's live
        // position, not the fixed point it occupied when the order was given. The break transition
        // uses DetectedHostileWithinRange, which ignores the escortee by faction (same faction as the
        // escort) rather than by geometry (#465) — so the escort and escortee must be spawned with
        // the same non-neutral faction, and only a hostile intruder trips the break.
        const float innerRange = standoffM * 0.5f;

        auto sm = std::make_unique<StateMachineController>(*entityManager);
        const fl::EntityManager* em = entityManager;
        sm->addState("follow", [em, escortedId, standoffM]() {
            return std::make_unique<DynamicLoiterController>(*em, escortedId, standoffM, 0.65f, LoiterDir::Clockwise);
        });
        sm->addState("break", []() { return std::make_unique<ImmelmannController>(); });
        // The escort breaks on a hostile it has actually SEEN (#690) — not on one it could not
        // possibly have noticed. Same fallback rule as patrol_attack when sensing is not evaluated.
        sm->addTransition("follow", "break", DetectedHostileWithinRange(innerRange));
        sm->addTransition("break", "follow", Not(DetectedHostileWithinRange(innerRange)), 6.0f);
        sm->setInitialState("follow");
        return sm;
    }

    // -----------------------------------------------------------------------
    // formation <anchorIdx> [slotIndex=0] [lateralM=150] [aftM=100]
    //
    // Hold station on a MOVING anchor. Unlike `escort` above (which orbits the point where the
    // escortee was standing when the order was given), this tracks the anchor continuously — it is
    // the controller `escort`'s own comment says is missing.
    // -----------------------------------------------------------------------
    if (behavior == "formation") {
        const fl::EntityId anchorId = c.requiredEntity(0);
        uint32_t slotIndex = 0;
        FormationParams fp{};
        c.optU32(1, slotIndex);
        c.optFloat(2, fp.lateralM);
        c.optFloat(3, fp.aftM);
        if (!c.ok())
            return nullptr;
        return std::make_unique<FormationController>(*entityManager, anchorId, slotIndex, fp);
    }

    // -----------------------------------------------------------------------
    // swarm <cx> <cy> <cz> [...] / swarm_follow <anchorIdx> [...]  (#353)
    //
    // One boids member. Spawn N entities of the same type and faction with this behavior and they
    // flock: separation/alignment/cohesion over the shared spatial index, migrating toward the
    // point (or after the anchor). Each member is independent — there is no swarm object to manage,
    // and losing members degrades the flock, never breaks it.
    // -----------------------------------------------------------------------
    if (behavior == "swarm" || behavior == "swarm_follow") {
        if (!entityManager)
            return nullptr;
        SwarmParams sp{};
        std::size_t tail = 0; // index of the first optional arg
        glm::dvec3 point{};
        fl::EntityId anchorId{};
        if (behavior == "swarm") {
            if (args.size() < 3)
                return nullptr;
            point = {c.requiredDouble(0), c.requiredDouble(1), c.requiredDouble(2)};
            tail = 3;
        } else {
            anchorId = c.requiredEntity(0);
            tail = 1;
        }
        c.optFloat(tail, sp.neighborRadiusM);
        c.optFloat(tail + 1, sp.separationRadiusM);
        c.optFloat(tail + 2, sp.cruiseThrottle);
        if (!c.ok())
            return nullptr;
        if (behavior == "swarm")
            return std::make_unique<SwarmController>(*entityManager, point, sp);
        return std::make_unique<SwarmController>(*entityManager, anchorId, sp);
    }

    // -----------------------------------------------------------------------
    // wingman <anchorIdx> <command> [slotIndex=0]
    //
    // Attach one of the six scripted wingman commands (WingmanCommand.h) directly, without a
    // formation roster. This is the game-master / debugging path and the same code the network order
    // path drives, so a console order and a radio order cannot behave differently.
    //
    // NOTE: `attack_my_target` has no designated target on this path (designation comes from the
    // commander's boresight, which the console does not have), so it degrades to holding station.
    // Use `pursuit`/`lead` to point an AI at a specific entity from the console.
    // -----------------------------------------------------------------------
    if (behavior == "wingman") {
        if (args.size() < 2)
            return nullptr;
        const fl::EntityId anchorId = c.requiredEntity(0);
        WingmanParams wp{};
        c.optU32(2, wp.slotIndex);
        if (!c.ok())
            return nullptr;

        const std::optional<WingmanCommand> cmd = parseWingmanCommand(args[1]);
        if (!cmd)
            return nullptr;

        // Home = the anchor's current position, so an RTB from the console goes somewhere sane.
        if (const fl::EntityState* as = entityManager->get(anchorId)) {
            wp.homePoint = glm::dvec3(as->transform.pos[0], as->transform.pos[1], as->transform.pos[2]);
        }
        return makeWingmanController(*entityManager, anchorId, *cmd, fl::EntityId{}, wp);
    }

    return nullptr;
}

} // namespace fl::ai
