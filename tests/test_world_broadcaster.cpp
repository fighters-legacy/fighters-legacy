// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_broadcaster_test_util.h"

using namespace fl;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The wiring behind #1076: the log stamps its own records, and this is the one place its tick
// advances. Deleting that line would put the whole event stream back at tick 0 for anything appended
// off the sim thread, so it gets a test rather than trust -- an event log whose timestamp silently
// stops moving is exactly the failure this issue existed to end.
TEST_CASE("WorldBroadcaster: onTick advances the match event log's tick (#1076)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    CHECK(broadcaster.matchEventLog().tick() == 0u);

    broadcaster.onTick(1.0 / 60.0, 41u);
    CHECK(broadcaster.matchEventLog().tick() == 41u);

    // An append with no tick of its own lands on the tick that is current, which is the AlertLevel
    // caller's exact shape.
    fl::MatchEvent alert;
    alert.type = fl::MatchEventType::AlertLevel;
    alert.factionIndex = 1;
    broadcaster.matchEventLog().append(std::move(alert));

    broadcaster.onTick(1.0 / 60.0, 42u);
    CHECK(broadcaster.matchEventLog().tick() == 42u);

    const auto events = broadcaster.matchEventLog().since(0);
    const auto it = std::find_if(events.begin(), events.end(),
                                 [](const fl::MatchEvent& e) { return e.type == fl::MatchEventType::AlertLevel; });
    REQUIRE(it != events.end());
    CHECK(it->tick == 41u);
}

TEST_CASE("WorldBroadcaster: replaceController swaps the controller preserving the integrator (#152)",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::EntityTransform t{};
    t.pos[1] = 3000.0;
    t.quat[3] = 1.f;
    const fl::EntityId id = em.spawn("builtin:debug-entity", t);
    broadcaster.registerController(id, std::make_unique<ConstantController>(), nullptr, /*airspeed=*/120.f,
                                   /*aiScriptName=*/"patrol");
    broadcaster.onTick(1.0 / 60.0, 1u);
    const fl::EntityState* s0 = em.get(id);
    REQUIRE(s0 != nullptr);
    const double vAfterTick = s0->transform.vel[0];

    // Tag query finds it.
    auto tagged = broadcaster.entitiesUsingAiScript("patrol");
    REQUIRE(tagged.size() == 1);
    CHECK(tagged[0].index == id.index);

    // Swap the controller; the live integrator (velocity) is preserved (not reset to a spawn state).
    auto slow = std::make_unique<ConstantController>();
    slow->throttle = 0.0f;
    CHECK(broadcaster.replaceController(id, std::move(slow), "reload"));
    broadcaster.onTick(1.0 / 60.0, 2u);
    const fl::EntityState* s1 = em.get(id);
    REQUIRE(s1 != nullptr);
    // Still moving forward from the preserved momentum (a rebuild-from-spawn would have zeroed it).
    CHECK(s1->transform.vel[0] > 0.5 * vAfterTick);

    // Re-tagged to the new script.
    CHECK(broadcaster.entitiesUsingAiScript("patrol").empty());
    CHECK(broadcaster.entitiesUsingAiScript("reload").size() == 1);

    // Replacing an unknown entity fails.
    CHECK_FALSE(broadcaster.replaceController(fl::EntityId{9999, 1}, std::make_unique<ConstantController>()));
}

TEST_CASE("WorldBroadcaster: an airborne spawn flies along its heading at t=0 (#883)", "[world_broadcaster][mission]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    // Heading yawed 90 deg about world up: body-forward (+X) maps to world -Z. A 90-deg quaternion is
    // {x, y, z, w} = {0, sin45, 0, cos45}.
    fl::EntityTransform t{};
    t.pos[1] = 3000.0;
    t.quat[0] = 0.f;
    t.quat[1] = 0.70710678f;
    t.quat[2] = 0.f;
    t.quat[3] = 0.70710678f;
    const fl::EntityId id = em.spawn("builtin:debug-entity", t);
    broadcaster.registerController(id, std::make_unique<ConstantController>(), nullptr, /*airspeed=*/100.f);
    broadcaster.onTick(1.0 / 60.0, 1u);

    const fl::EntityState* s = em.get(id);
    REQUIRE(s != nullptr);
    // Before #883 the integrator started at zero body velocity AND identity orientation, so the aircraft
    // sat at ~0 airspeed pointing +X and tumbled. Now it is seeded with 100 m/s along its actual heading
    // (world -Z), i.e. in stable forward flight at t=0.
    CHECK(s->transform.vel[2] < -80.f); // moving along the heading (world -Z), ~100 m/s
    CHECK(s->transform.vel[0] > -30.f); // not flung along +X (which identity orientation gave)
    CHECK(s->transform.vel[0] < 30.f);

    // A ground/sandbox spawn (airspeed 0) stays put — no phantom velocity.
    fl::EntityTransform t2{};
    t2.pos[1] = 3000.0;
    t2.quat[3] = 1.f;
    const fl::EntityId still = em.spawn("builtin:debug-entity", t2);
    broadcaster.registerController(still, std::make_unique<ConstantController>(), nullptr, /*airspeed=*/0.f);
    broadcaster.onTick(1.0 / 60.0, 2u);
    const fl::EntityState* s2 = em.get(still);
    REQUIRE(s2 != nullptr);
    CHECK(s2->transform.vel[0] > -20.f);
    CHECK(s2->transform.vel[0] < 20.f);
    CHECK(s2->transform.vel[2] > -20.f);
    CHECK(s2->transform.vel[2] < 20.f);
}

TEST_CASE("WorldBroadcaster::ejectPilot spawns a parachute and resolves the pilot outcome (#672)",
          "[world_broadcaster][ejection]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(fl::builtinParachuteDef());
    fl::EntityManager em(logger, registry);
    // A query is frozen at construction (#1082), so the per-SECTION value is a variable the query
    // reads at call time rather than two different queries installed after the fact.
    // Neutral is what an UNSET query means, so the sections that never touch this see the plain-server
    // behaviour they saw when no query was installed at all.
    fl::TerritoryControl territory = fl::TerritoryControl::Neutral;
    fl::WorldQueries q_broadcaster;
    q_broadcaster.territory = [&territory](glm::dvec3, uint16_t) { return territory; };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));
    broadcaster.setParachuteType("builtin:parachute");

    const uint32_t chuteType = registry.indexById("builtin:parachute");
    auto countParachutes = [&]() {
        int n = 0;
        em.forEach([&](const fl::EntityState& s) {
            if (!s.dead && s.typeIndex == chuteType)
                ++n;
        });
        return n;
    };

    SECTION("high and slow: the seat saves the pilot (MIA on a plain server) and a chute replicates") {
        fl::EntityTransform t{};
        t.pos[1] = 3000.0;
        t.quat[3] = 1.f;
        const fl::EntityId id = em.spawn("builtin:debug-entity", t);
        const fl::EjectionOutcome outcome = broadcaster.ejectPilot(id);
        CHECK(outcome == fl::EjectionOutcome::MIA); // survived, no frontline -> MIA
        const fl::EntityState* air = em.get(id);
        CHECK((air == nullptr || air->dead)); // the airframe is lost
        CHECK(countParachutes() == 1);        // a replicating chute was spawned
    }

    SECTION("low and diving: the seat cannot save the pilot (KIA), the airframe is still lost") {
        fl::EntityTransform t{};
        t.pos[1] = 15.0; // 15 m AGL
        t.quat[3] = 1.f;
        const fl::EntityId id = em.spawn("builtin:debug-entity", t);
        if (fl::EntityState* st = em.get(id))
            st->transform.vel[1] = -60.f; // diving at 60 m/s: the chute cannot deploy in 15 m
        const fl::EjectionOutcome outcome = broadcaster.ejectPilot(id);
        CHECK(outcome == fl::EjectionOutcome::KIA);
        const fl::EntityState* air = em.get(id);
        CHECK((air == nullptr || air->dead));
    }

    SECTION("ejecting an already-dead entity is a no-op KIA") {
        fl::EntityTransform t{};
        t.pos[1] = 3000.0;
        t.quat[3] = 1.f;
        const fl::EntityId id = em.spawn("builtin:debug-entity", t);
        em.kill(id);
        CHECK(broadcaster.ejectPilot(id) == fl::EjectionOutcome::KIA);
        CHECK(countParachutes() == 0); // no chute for a corpse
    }

    SECTION("the injected territory query decides a survivor's fate (#672)") {
        auto spawnHigh = [&]() {
            fl::EntityTransform t{};
            t.pos[1] = 3000.0;
            t.quat[3] = 1.f;
            return em.spawn("builtin:debug-entity", t);
        };
        territory = fl::TerritoryControl::Friendly;
        CHECK(broadcaster.ejectPilot(spawnHigh()) == fl::EjectionOutcome::Rescued);
        territory = fl::TerritoryControl::Hostile;
        CHECK(broadcaster.ejectPilot(spawnHigh()) == fl::EjectionOutcome::Captured);
    }
}

TEST_CASE("WorldBroadcaster: an AI pilot auto-ejects when critically hit (#672)", "[world_broadcaster][ejection]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(fl::builtinParachuteDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setParachuteType("builtin:parachute");
    broadcaster.setAiAutoEject(true); // opt in (off by default so plain damage tests are unaffected)
    const uint32_t chuteType = registry.indexById("builtin:parachute");

    fl::EntityTransform t{};
    t.pos[1] = 4000.0;
    t.quat[3] = 1.f;
    const fl::EntityId id = em.spawn("builtin:debug-entity", t);
    broadcaster.registerController(id, std::make_unique<ConstantController>(), nullptr, /*airspeed=*/100.f);

    // Healthy: a tick does NOT eject.
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK((em.get(id) != nullptr && !em.get(id)->dead));

    // Critically hit (below the 15% auto-eject threshold): the next tick punches the AI pilot out.
    if (fl::EntityState* st = em.get(id))
        st->hp = 0.1f * st->maxHp;
    broadcaster.onTick(1.0 / 60.0, 2u);

    const fl::EntityState* air = em.get(id);
    CHECK((air == nullptr || air->dead)); // airframe lost
    int chutes = 0;
    em.forEach([&](const fl::EntityState& s) {
        if (!s.dead && s.typeIndex == chuteType)
            ++chutes;
    });
    CHECK(chutes == 1); // a replicating chute was spawned exactly once
}

TEST_CASE("WorldBroadcaster: registerController steps a non-peer entity and serializes it", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 1000.0;
    fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    // Connect a peer so snapshots are sent (per-peer unicast model requires at least one peer).
    connectPilotPeer(broadcaster, net, 0u);

    // Register an AI/scripted controller for the pre-spawned entity.
    auto controller = std::make_unique<ConstantController>();
    ConstantController* ctrlPtr = controller.get();
    broadcaster.registerController(id, std::move(controller));

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    // The controller was sampled once per tick — proof the non-peer entity is stepped.
    CHECK(ctrlPtr->sampleCount == 120);

    // The entity moved under its own controller (full-throttle builtin model accelerates forward).
    const fl::EntityState* st = em.get(id);
    REQUIRE(st != nullptr);
    const bool moved = st->transform.pos[0] != 0.0 || st->transform.pos[2] != 0.0 || st->transform.pos[1] != 1000.0;
    CHECK(moved);

    // It serializes into the peer's snapshot with live throttle telemetry.
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps.back();
    auto hdr = parseSnapshotHeader(pkt);
    REQUIRE(totalEntityCount(hdr) >= 1u);
    // AI entity appears in full entries (the peer never acks, so records stay full every tick)
    bool foundAiEntity = false;
    for (const auto& e : parseFullEntries(pkt)) {
        if (e.entityIdx == id.index) {
            CHECK(e.throttle > 0u); // throttle spooled up toward the commanded 100%
            foundAiEntity = true;
        }
    }
    CHECK(foundAiEntity);
}

TEST_CASE("WorldBroadcaster: reaps an orphaned controller when its entity is destroyed (#702)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 1000.0;
    fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.registerController(id, std::make_unique<ConstantController>());
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(broadcaster.controlledEntityCount() == 1);

    // Kill the entity outside onDisconnect (combat / AI arrival despawn / a Lua despawn). Once the
    // EntityManager reaps the dead slot, the next gather must drop the now-orphaned controller instead
    // of leaving it to churn until the pool index is reused.
    em.kill(id);
    for (uint64_t tick = 2; tick <= 4; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);
    CHECK(broadcaster.controlledEntityCount() == 0);
}

// Run a fixed multi-entity scenario and capture final entity state. When `jobs` is non-null the
// per-entity AI + integrate passes run data-parallel; otherwise they run inline. Used to prove the
// parallel path is serial-equivalent.
namespace {
struct FinalState {
    uint32_t idx;
    double pos[3];
    float quat[4];
};

std::vector<FinalState> runParallelScenario(fl::JobSystem* jobs) {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WeatherController weather;
    weather.setPreset(fl::WeatherPreset::Storm); // turbulence on -> exercises the per-entity RNG

    fl::WorldBroadcaster broadcaster(em, registry, net, logger, &weather);
    if (jobs)
        broadcaster.setJobSystem(*jobs);
    // Disable the overrun governor (#514): it reads wall-clock tick time, so leaving it enabled would
    // let a slow CI tick perturb AI sampling differently across the worker-count runs and break the
    // bit-identity claim. The AI-decimation lever's own serial-equivalence is proven separately below.
    {
        fl::TickGovernorParams gp;
        gp.enabled = false;
        broadcaster.setGovernorParams(gp);
    }
    connectPilotPeer(broadcaster, net, 0u);

    std::vector<fl::EntityId> ids;
    for (int i = 0; i < 16; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 100.0;
        t.pos[1] = 1000.0;
        t.pos[2] = i * 50.0;
        fl::EntityId id = em.spawn("builtin:debug-entity", t);
        REQUIRE(id.valid());
        auto controller = std::make_unique<ConstantController>();
        controller->throttle = 1.0f;
        broadcaster.registerController(id, std::move(controller));
        ids.push_back(id);
    }

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    std::vector<FinalState> out;
    for (fl::EntityId id : ids) {
        const fl::EntityState* st = em.get(id);
        REQUIRE(st != nullptr);
        FinalState fsv{};
        fsv.idx = id.index;
        for (int k = 0; k < 3; ++k)
            fsv.pos[k] = st->transform.pos[k];
        for (int k = 0; k < 4; ++k)
            fsv.quat[k] = st->transform.quat[k];
        out.push_back(fsv);
    }
    std::sort(out.begin(), out.end(), [](const FinalState& a, const FinalState& b) { return a.idx < b.idx; });
    return out;
}
} // namespace

TEST_CASE("WorldBroadcaster: parallel sim tick is serial-equivalent across worker counts", "[world_broadcaster]") {
    const std::vector<FinalState> baseline = runParallelScenario(nullptr); // inline / serial reference

    for (unsigned total : {1u, 2u, 8u}) {
        fl::JobSystem jobs(total);
        const std::vector<FinalState> got = runParallelScenario(&jobs);
        REQUIRE(got.size() == baseline.size());
        for (size_t i = 0; i < baseline.size(); ++i) {
            CHECK(got[i].idx == baseline[i].idx);
            // Bit-identical, not Approx: each entity integrates independently with no cross-entity
            // reduction, so parallelism must not change a single bit.
            for (int k = 0; k < 3; ++k)
                CHECK(got[i].pos[k] == baseline[i].pos[k]);
            for (int k = 0; k < 4; ++k)
                CHECK(got[i].quat[k] == baseline[i].quat[k]);
        }
    }
}

// Run a fixed multi-peer scenario and capture every per-peer WorldSnapshot packet, keyed by peerId.
// When `jobs` is non-null the per-peer snapshot assembly runs data-parallel via runPeerPass; else it
// runs inline. Used to prove the parallel snapshot build is byte-identical to the serial path. When
// `killAtTick > 0`, one shared entity is killed at that tick so the per-peer despawn-detection +
// SnapshotDespawn TLV path is exercised under parallelism.
namespace {} // namespace

// -----------------------------------------------------------------------------------------------
// Graceful tick-overrun governor (#514).
// -----------------------------------------------------------------------------------------------

namespace {

struct DecimationResult {
    std::vector<FinalState> states;
    uint32_t finalAiStride{1};
    float finalLoadFactor{1.f};
};

// Run a fixed AI-entity scenario under the auto-advancing (over-budget) clock so the governor sheds.
// When `jobs` is non-null the per-entity passes run data-parallel. Used to prove the AI-sample
// decimation lever is serial-equivalent across worker counts.
DecimationResult runDecimationScenario(fl::JobSystem* jobs) {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    AutoAdvanceClock clock(std::chrono::milliseconds(3)); // ~constant over-budget tick span

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    if (jobs)
        broadcaster.setJobSystem(*jobs);
    // Aggressive, fast-reacting governor: evaluate every tick so it reaches the floor quickly.
    fl::TickGovernorParams gp = fl::makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u);
    gp.evalIntervalTicks = 1u;
    gp.ewmaAlpha = 1.0f;
    broadcaster.setGovernorParams(gp);
    connectPilotPeer(broadcaster, net, 0u);

    std::vector<fl::EntityId> ids;
    for (int i = 0; i < 16; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 100.0;
        t.pos[1] = 1000.0;
        t.pos[2] = i * 50.0;
        fl::EntityId id = em.spawn("builtin:debug-entity", t);
        REQUIRE(id.valid());
        auto controller = std::make_unique<ConstantController>();
        controller->throttle = 1.0f;
        broadcaster.registerController(id, std::move(controller)); // decimatable (AI)
        ids.push_back(id);
    }

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    DecimationResult res;
    const fl::OverrunStatus ov = broadcaster.getOverrunStatus();
    res.finalAiStride = ov.aiStride;
    res.finalLoadFactor = ov.loadFactor;
    for (fl::EntityId id : ids) {
        const fl::EntityState* st = em.get(id);
        REQUIRE(st != nullptr);
        FinalState fsv{};
        fsv.idx = id.index;
        for (int k = 0; k < 3; ++k)
            fsv.pos[k] = st->transform.pos[k];
        for (int k = 0; k < 4; ++k)
            fsv.quat[k] = st->transform.quat[k];
        res.states.push_back(fsv);
    }
    std::sort(res.states.begin(), res.states.end(),
              [](const FinalState& a, const FinalState& b) { return a.idx < b.idx; });
    return res;
}
} // namespace

TEST_CASE("WorldBroadcaster: AI-sample decimation is serial-equivalent across worker counts",
          "[world_broadcaster][overrun]") {
    const DecimationResult baseline = runDecimationScenario(nullptr); // inline reference
    // The governor must actually have engaged AI decimation, else the test is vacuous.
    REQUIRE(baseline.finalAiStride > 1u);
    REQUIRE(baseline.finalLoadFactor < 1.0f);

    for (unsigned total : {1u, 2u, 8u}) {
        fl::JobSystem jobs(total);
        const DecimationResult got = runDecimationScenario(&jobs);
        CHECK(got.finalAiStride == baseline.finalAiStride);
        REQUIRE(got.states.size() == baseline.states.size());
        for (size_t i = 0; i < baseline.states.size(); ++i) {
            CHECK(got.states[i].idx == baseline.states[i].idx);
            // Bit-identical: the skip predicate (tickIndex+idx)%stride and per-entity lastInput reuse
            // are pure functions of (idx, tick, stride) with only disjoint per-entity writes.
            for (int k = 0; k < 3; ++k)
                CHECK(got.states[i].pos[k] == baseline.states[i].pos[k]);
            for (int k = 0; k < 4; ++k)
                CHECK(got.states[i].quat[k] == baseline.states[i].quat[k]);
        }
    }
}

// Run a fixed multi-peer scenario under the over-budget clock with the interest-radius lever engaged
// (#726): entities spread out to ~150 km so the floor-scaled radius (200 km x 0.5) actually changes
// each peer's visible set. Captures every per-peer snapshot packet + the final governor state; used
// to prove the radius lever preserves the byte-identical serial-equivalence of the peer pass.
namespace {} // namespace

TEST_CASE("WorldBroadcaster: getTickBudget records per-phase timing after onTick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 1000.0;
    fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.registerController(id, std::make_unique<ConstantController>());

    // Before any tick, nothing is sampled.
    CHECK(broadcaster.getTickBudget().ticksSampled == 0u);

    for (uint64_t tick = 1; tick <= 5; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const fl::TickBudget tb = broadcaster.getTickBudget();
    CHECK(tb.ticksSampled == 5u);
    CHECK(tb.ticksTotal == 5u);
    // Timing magnitudes are environment-dependent; assert only that every phase is finite and >= 0
    // and that the integrate/ai split wiring populated the accumulators without NaN/negatives.
    CHECK(std::isfinite(tb.total.mean));
    CHECK(tb.total.mean >= 0.0);
    CHECK(tb.tickHz >= 0.0);
    CHECK(std::isfinite(tb.tickHz));
    for (int i = 0; i < fl::kTickPhaseCount; ++i) {
        CHECK(std::isfinite(tb.phases[i].mean));
        CHECK(tb.phases[i].mean >= 0.0);
    }
    CHECK(tb.other.mean >= 0.0);
}

TEST_CASE("WorldBroadcaster: flight model resolver is consulted for a flightModelAsset, falls back on miss",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    fl::EntityDef def = makeDebugDef();
    def.flightModelAsset = "models/x";
    registry.registerType(def);

    std::string requestedId;
    fl::WorldQueries q_broadcaster;
    q_broadcaster.flightModel = [&](const std::string& id) -> std::shared_ptr<const fl::FlightModelData> {
        requestedId = id;
        return nullptr; // unknown id -> WorldBroadcaster falls back to the builtin model
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(requestedId == "models/x"); // resolver consulted with the entity's flightModelAsset
    CHECK(em.liveCount() == 1u);      // spawn still succeeded via the builtin fallback
}

TEST_CASE("WorldBroadcaster: flight model resolver is skipped when flightModelAsset is empty", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef()); // empty flightModelAsset

    bool called = false;
    fl::WorldQueries q_broadcaster;
    q_broadcaster.flightModel = [&](const std::string&) -> std::shared_ptr<const fl::FlightModelData> {
        called = true;
        return nullptr;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK_FALSE(called); // empty id -> resolver never invoked
    CHECK(em.liveCount() == 1u);
}

TEST_CASE("WorldBroadcaster: onTick with no peers sends nothing", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    // No onConnect — per-peer loop iterates over an empty m_peerEntities; no snapshot is sent.
    broadcaster.onTick(1.0 / 60.0, 5u);

    CHECK(snapshotsFor(net, 0).empty());
}

TEST_CASE("WorldBroadcaster: onTick with connected peer and no extra entities sends peer-only snapshot",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 5u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    auto hdr = parseSnapshotHeader(pkt);
    CHECK(totalEntityCount(hdr) == 1u); // only the peer's own entity
    CHECK(hdr.tickIndex == 5u);

    // Packet = header + quantized bitstream (one full own-entity record) + SnapshotPeerCount TLV
    // (6 bytes) + the parked pilot's articulation record. SnapshotPeerLatency is absent
    // (estimatedDelayTicks == 0; no heartbeat sent).
    CHECK(hdr.bitstreamBytes > 0u);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) +
                                  static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() == extOffset + 6u + kParkedGearArtTlvBytes);
    uint16_t pc{};
    CHECK(fl::readExtValue(pkt.data() + extOffset, 6u, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
    CHECK(pc == 1u); // 1 active peer
}

TEST_CASE("WorldBroadcaster: match roster broadcasts joins, sanitizes callsigns, sends leaves (#996)",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    SECTION("join broadcast + self roster + sanitization") {
        connectPilotWithCallsign(broadcaster, 0u, "  Maverick  "); // padded -> trimmed
        // The joiner sees itself (via the upsert broadcast + full-roster send).
        std::vector<fl::PlayerRosterEntry> mine = collectRosterFor(net, 0u);
        bool sawSelf = false;
        for (const auto& e : mine)
            if (e.participantId == 0u && std::string(e.callsign) == "Maverick")
                sawSelf = true;
        CHECK(sawSelf);

        // A second pilot joins: peer 0 must learn of peer 1, and peer 1's full roster includes peer 0.
        const std::size_t before = net.perPeerSends.size();
        connectPilotWithCallsign(broadcaster, 1u, "Goose");
        (void)before;
        std::vector<fl::PlayerRosterEntry> p0 = collectRosterFor(net, 0u);
        bool p0SawP1 = false;
        for (const auto& e : p0)
            if (e.participantId == 1u && std::string(e.callsign) == "Goose")
                p0SawP1 = true;
        CHECK(p0SawP1);
        std::vector<fl::PlayerRosterEntry> p1 = collectRosterFor(net, 1u);
        bool p1SawP0 = false;
        for (const auto& e : p1)
            if (e.participantId == 0u && std::string(e.callsign) == "Maverick")
                p1SawP0 = true;
        CHECK(p1SawP0);
    }

    SECTION("empty callsign falls back to Pilot-<id>") {
        connectPilotWithCallsign(broadcaster, 5u, "");
        std::vector<fl::PlayerRosterEntry> mine = collectRosterFor(net, 5u);
        bool ok = false;
        for (const auto& e : mine)
            if (e.participantId == 5u && std::string(e.callsign) == "Pilot-5")
                ok = true;
        CHECK(ok);
    }

    SECTION("control characters are stripped from a callsign") {
        connectPilotWithCallsign(broadcaster, 2u,
                                 "Ba\x01\x02"
                                 "d\x7f"); // split literal: \x is greedy over hex
        std::vector<fl::PlayerRosterEntry> mine = collectRosterFor(net, 2u);
        bool ok = false;
        for (const auto& e : mine)
            if (e.participantId == 2u && std::string(e.callsign) == "Bad")
                ok = true;
        CHECK(ok);
    }

    SECTION("disconnect broadcasts a leave to remaining peers") {
        connectPilotWithCallsign(broadcaster, 0u, "Maverick");
        connectPilotWithCallsign(broadcaster, 1u, "Goose");
        net.perPeerSends.clear();
        broadcaster.onDisconnect(1u);
        std::vector<fl::PlayerRosterEntry> p0 = collectRosterFor(net, 0u);
        bool leaveSeen = false;
        for (const auto& e : p0)
            if (e.participantId == 1u && (e.flags & fl::kRosterLeave))
                leaveSeen = true;
        CHECK(leaveSeen);
    }
}

TEST_CASE("WorldBroadcaster: respawn enrolls on death and respawnParticipant respawns (#648)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    em.addEventHandler(&broadcaster);

    fl::WorldBroadcaster::RespawnPolicy policy;
    policy.delayTicks = 5;
    broadcaster.setRespawnPolicy(policy);

    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    broadcaster.onTick(1.0 / 60.0, 1u);
    REQUIRE(em.liveCount() == 1u);

    // Kill the pilot's aircraft. The Died event enrolls a respawn; the entity teardown is deferred.
    em.applyDamage({ack.assignedEntityIdx, ack.assignedEntityGen}, 200.f, fl::EntityId::null());
    broadcaster.onTick(1.0 / 60.0, 2u); // Died fires; processRespawns cleans up the dead entity
    broadcaster.onTick(1.0 / 60.0, 3u);
    CHECK(em.liveCount() == 0u);             // dead, awaiting respawn
    CHECK(broadcaster.getPeerCount() == 1u); // still connected

    // Force respawn (the admin path) brings the pilot back with a fresh aircraft.
    broadcaster.respawnParticipant(0u);
    broadcaster.onTick(1.0 / 60.0, 4u);
    CHECK(em.liveCount() == 1u);
}

TEST_CASE("WorldBroadcaster: bot participants get a roster row and are removable (#87)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u); // a human peer to receive the roster broadcast
    fl::EntityId botEnt = em.spawn("builtin:debug-entity", fl::EntityTransform{});
    REQUIRE(botEnt.valid());
    const uint32_t botPid = fl::kBotParticipantBase + 3u;
    net.perPeerSends.clear();
    broadcaster.registerBotParticipant(botPid, botEnt, "Viper-1", 2u);

    // Peer 0 received a roster upsert for the bot, flagged as a bot.
    std::vector<fl::PlayerRosterEntry> rows = collectRosterFor(net, 0u);
    bool sawBot = false;
    for (const auto& e : rows)
        if (e.participantId == botPid && (e.flags & fl::kRosterBot) && std::string(e.callsign) == "Viper-1")
            sawBot = true;
    CHECK(sawBot);

    // Removing the bot broadcasts a leave.
    net.perPeerSends.clear();
    broadcaster.removeBotParticipant(botPid);
    rows = collectRosterFor(net, 0u);
    bool leave = false;
    for (const auto& e : rows)
        if (e.participantId == botPid && (e.flags & fl::kRosterLeave))
            leave = true;
    CHECK(leave);
}

TEST_CASE("WorldBroadcaster: match state + event sink + resetWorld (#523)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    SECTION("setMatchState broadcasts and a late joiner is unicast the state") {
        connectPilotPeer(broadcaster, net, 0u);
        fl::WorldBroadcaster::MatchStatePod pod;
        pod.phase = 2; // Active
        pod.scoreLimit = 50;
        pod.phaseEndTick = 900;
        pod.modeId = "builtin:tdm";
        pod.modeName = "Team Deathmatch";
        pod.teamScores = {{1, 3}, {2, 5}};
        net.perPeerSends.clear();
        broadcaster.setMatchState(pod);
        // Peer 0 received a MatchState packet.
        bool got = false;
        for (const auto& [pid, pkt] : net.perPeerSends)
            if (pid == 0u && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::MatchState))
                got = true;
        CHECK(got);
        // A late joiner is unicast the current state.
        net.perPeerSends.clear();
        connectPilotPeer(broadcaster, net, 1u);
        bool lateGot = false;
        for (const auto& [pid, pkt] : net.perPeerSends)
            if (pid == 1u && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::MatchState))
                lateGot = true;
        CHECK(lateGot);
    }

    SECTION("resetWorld despawns entities but keeps peers connected") {
        connectPilotPeer(broadcaster, net, 0u);
        broadcaster.onTick(1.0 / 60.0, 1u);
        REQUIRE(em.liveCount() == 1u);
        broadcaster.resetWorld();
        broadcaster.onTick(1.0 / 60.0, 2u);
        CHECK(em.liveCount() == 0u);             // entity gone
        CHECK(broadcaster.getPeerCount() == 1u); // peer still connected
        // Re-admit spawns a fresh aircraft.
        broadcaster.readmitPilots();
        broadcaster.onTick(1.0 / 60.0, 3u);
        CHECK(em.liveCount() == 1u);
    }
}

TEST_CASE("WorldBroadcaster: MsgTeamRequest honors the switch guard (#522)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldQueries q_broadcaster;
    fl::WorldBroadcasterHooks h_broadcaster;
    q_broadcaster.teamAssigner = [](uint32_t) -> std::optional<uint16_t> { return uint16_t{1}; };
    // Frozen at construction (#1082): the SECTION sets the verdict, not the guard.
    bool switchAllowed = false;
    h_broadcaster.match.teamSwitchGuard = [&switchAllowed](uint32_t, uint16_t) { return switchAllowed; };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster),
                                     std::move(h_broadcaster));
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);
    REQUIRE(broadcaster.factionForPeer(0u) == 1u);

    SECTION("a denied switch keeps the current team and sends a notice") {
        switchAllowed = false;
        net.sends.clear();
        fl::MsgTeamRequest req{};
        req.factionIndex = 2;
        broadcaster.onReceive(0u, &req, sizeof(req));
        broadcaster.onTick(1.0 / 60.0, 2u);
        CHECK(broadcaster.factionForPeer(0u) == 1u); // unchanged
        bool notice = false;
        for (const auto& pkt : net.sends)
            if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
                notice = true;
        CHECK(notice);
    }

    SECTION("an allowed switch respawns on the new team") {
        switchAllowed = true;
        fl::MsgTeamRequest req{};
        req.factionIndex = 2;
        broadcaster.onReceive(0u, &req, sizeof(req));
        broadcaster.onTick(1.0 / 60.0, 2u);
        CHECK(broadcaster.factionForPeer(0u) == 2u);
    }
}

namespace {} // namespace

TEST_CASE("WorldBroadcaster: setSpectateTarget rejects unknown peers (#403)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK_FALSE(broadcaster.setSpectateTarget(99u, 0u));                             // unknown peer
    CHECK(broadcaster.setSpectateTarget(0u, 5u));                                    // known peer, set
    CHECK(broadcaster.setSpectateTarget(0u, fl::PeerInputState::kNoSpectateTarget)); // off
}

TEST_CASE("WorldBroadcaster: onDisconnect after connect removes peer entity", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    // liveCount() is updated by onTick — drive a tick to confirm the spawn landed.
    broadcaster.onTick(1.0 / 60.0, 0u);
    REQUIRE(em.liveCount() == 1u);

    broadcaster.onDisconnect(0u);
    // Entity is marked dead and reaped on the next onTick.
    const std::size_t sendsBefore = net.sends.size(); // includes Hello + ConnectAck + snapshot from tick 0
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 0u);
    CHECK(net.sends.size() == sendsBefore); // nothing extra after disconnect (no peer left)
}

TEST_CASE("WorldBroadcaster: onDisconnect does not crash and sends nothing", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    REQUIRE_NOTHROW(broadcaster.onDisconnect(0u));
    CHECK(net.sends.empty());
    CHECK(snapshotsFor(net, 0).empty());
}

TEST_CASE("WorldBroadcaster: onReceive is a no-op for unknown msgId", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE_NOTHROW(broadcaster.onReceive(0u, garbage, sizeof(garbage)));
    CHECK(net.sends.empty());
    CHECK(snapshotsFor(net, 0).empty());
}

TEST_CASE("WorldBroadcaster: onReceive empty packet is discarded", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    clearSnapshots(net);
    net.sends.clear();

    broadcaster.onReceive(0u, nullptr, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Entity should not have moved (no input applied).
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // Empty packet discarded: entity is present and data is finite (craft still flies).
    CHECK(std::isfinite(e.vel[0]));
    CHECK(std::isfinite(e.vel[1]));
    CHECK(std::isfinite(e.vel[2]));
}

TEST_CASE("WorldBroadcaster: onReceive valid ClientInput moves entity on next tick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f; // full throttle
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // Entity starts with identity orientation (+X forward); throttle=1 should produce
    // non-zero velocity in the +X direction.
    CHECK(e.vel[0] > 0.f);
}

TEST_CASE("WorldBroadcaster: onReceive truncated ClientInput is discarded", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    clearSnapshots(net);

    // Only 10 bytes — less than sizeof(MsgClientInput), so it must be discarded as truncated.
    const uint8_t tiny[] = {static_cast<uint8_t>(fl::MsgId::ClientInput), 0, 0, 0, 0, 0, 0, 0, 0, 0};
    broadcaster.onReceive(0u, tiny, sizeof(tiny));
    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // Truncated packet discarded: entity is present and velocity is finite.
    CHECK(std::isfinite(e.vel[0]));
    CHECK(std::isfinite(e.vel[1]));
    CHECK(std::isfinite(e.vel[2]));
}

TEST_CASE("WorldBroadcaster: onReceive clamps out-of-range throttle", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    clearSnapshots(net);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 5.f; // out-of-range; must be clamped to 1.0
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // Throttle clamped to 1.0 (not 5.0): craft accelerates forward without exceeding physics limits.
    CHECK(e.vel[0] > 0.f);
    CHECK(std::isfinite(e.vel[1]));
}

TEST_CASE("WorldBroadcaster: onReceive zero viewAxis uses forward fallback", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    clearSnapshots(net);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    inp.viewAxis[0] = 0.f; // degenerate — all zero
    inp.viewAxis[1] = 0.f;
    inp.viewAxis[2] = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    broadcaster.onTick(1.0 / 60.0, 1u);

    // Entity should still move (fallback viewAxis {1,0,0} used for normalisation;
    // actual kinematics uses entity quaternion, not viewAxis directly, so entity moves).
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    CHECK(e.vel[0] > 0.f); // entity moves forward (+X) with identity orientation
}

TEST_CASE("WorldBroadcaster: onReceive after peer disconnects has no effect on tick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u); // entity killed; maps cleared

    // Client sends an input after disconnecting — server re-adds to m_peerInputs only.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    // onTick: kinematics loop finds peerId 0 in m_peerInputs but NOT in m_peerEntities -> skip.
    REQUIRE_NOTHROW(broadcaster.onTick(1.0 / 60.0, 1u));
    // Entity was reaped; liveCount = 0.
    CHECK(em.liveCount() == 0u);
}

TEST_CASE("WorldBroadcaster: onTick skips kinematics for dead entity", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Kill the entity externally — marks dead=true, queues reap.
    // Entity remains in m_peerEntities (disconnect hasn't happened).
    fl::MsgConnectAck ack = parseSendAck(net);
    fl::EntityId id;
    id.index = ack.assignedEntityIdx;
    id.generation = ack.assignedEntityGen;
    em.kill(id);

    // onTick: kinematics loop calls get(id) -> state->dead == true -> skip.
    clearSnapshots(net);
    REQUIRE_NOTHROW(broadcaster.onTick(1.0 / 60.0, 1u));
}

TEST_CASE("WorldBroadcaster: two peers each control independent entities", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);

    // Peer 0: full throttle forward; peer 1: no throttle.
    // Set inputs BEFORE the first tick. On tick 0 all entities are new to their peers, so they
    // appear as full records — this lets us verify velocity via parseFullEntries.
    fl::MsgClientInput inp0{};
    inp0.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp0.seqNum = 1u;
    inp0.throttle = 1.f;
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    fl::MsgClientInput inp1{};
    inp1.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp1.seqNum = 1u;
    inp1.throttle = 0.f;
    broadcaster.onReceive(1u, &inp1, sizeof(inp1));

    broadcaster.onTick(1.0 / 60.0, 0u); // tick 0: all entities new → full entries
    REQUIRE(em.liveCount() == 2u);

    // Snapshot has both entities; find peer 0's and peer 1's entries by assigned idx. The handshake
    // interleaves several sends per peer (Hello, ConnectAck, type defs, roster, ...), so scan for the
    // ConnectAcks by id/size in order rather than assuming fixed indices — peer 0 is admitted fully
    // before peer 1, so the first ConnectAck is peer 0's and the second is peer 1's. (Indexing a fixed
    // slot read past a 4-byte Hello/roster packet once #996 added the roster send — an OOB read ASan/TSan
    // flags even though the decoded values happened to still satisfy the asserts.)
    std::vector<fl::MsgConnectAck> acks;
    for (const auto& pkt : net.sends)
        if (pkt.size() >= sizeof(fl::MsgConnectAck) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck)) {
            fl::MsgConnectAck a;
            std::memcpy(&a, pkt.data(), sizeof(a));
            acks.push_back(a);
        }
    REQUIRE(acks.size() >= 2u);
    const fl::MsgConnectAck ack0 = acks[0];
    const fl::MsgConnectAck ack1 = acks[1];

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    CHECK(totalEntityCount(hdr) == 2u);

    // Find entries for each peer's entity by index within peer 0's snapshot.
    // With default draw distance (200 km) both entities are visible to peer 0.
    DecodedEntity ePeer0{}, ePeer1{};
    for (const auto& e : parseFullEntries(pkt)) {
        if (e.entityIdx == ack0.assignedEntityIdx)
            ePeer0 = e;
        else if (e.entityIdx == ack1.assignedEntityIdx)
            ePeer1 = e;
    }

    // Peer 0 (throttle=1) accelerates via thrust; peer 1 (throttle=0) decelerates via drag.
    // After one tick from the same initial 40 m/s, peer 0 must be faster than peer 1.
    CHECK(ePeer0.vel[0] > 0.f);
    CHECK(ePeer0.vel[0] > ePeer1.vel[0]);
    CHECK(std::isfinite(ePeer1.vel[0]));
    CHECK(std::isfinite(ePeer1.vel[1]));
}

TEST_CASE("WorldBroadcaster: onReceive discards MsgClientInput with mismatched protocolVersion",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Send a full-throttle input with the wrong protocol version.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    inp.protocolVersion = 0xFFFFu; // deliberate mismatch
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // Mismatched version discarded: default throttle=0 input used instead of throttle=1.
    // Full throttle from 40 m/s produces ~40.6 m/s in one tick; this must stay < 40.1 m/s.
    CHECK(e.vel[0] < 40.1f);
}

// ---------------------------------------------------------------------------
// getPeerCount
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: getPeerCount is zero before any connections", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    CHECK(broadcaster.getPeerCount() == 0);
}

TEST_CASE("WorldBroadcaster: getPeerCount tracks connect and disconnect", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    CHECK(broadcaster.getPeerCount() == 0);

    connectPilotPeer(broadcaster, net, 42);
    CHECK(broadcaster.getPeerCount() == 1);

    broadcaster.onDisconnect(42);
    CHECK(broadcaster.getPeerCount() == 0);
}

TEST_CASE("WorldBroadcaster: engineFailFlags has kEngineFailGeneric when entity damage is Heavy",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgConnectAck ack = parseSendAck(net);
    fl::EntityId id;
    id.index = ack.assignedEntityIdx;
    id.generation = ack.assignedEntityGen;
    auto* state = em.get(id);
    REQUIRE(state != nullptr);
    state->damageLevel = fl::DamageLevel::Heavy; // >= 2 → kEngineFailGeneric OR'd in

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);

    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    CHECK((e.engineFailFlags & fl::kEngineFailGeneric) != 0u);
}

// ---------------------------------------------------------------------------
// seqNum staleness guard and delay estimation
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: onReceive discards duplicate seqNum", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // First packet (seqNum=5, throttle=0) accepted.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 5u;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Duplicate seqNum=5 with throttle=1 must be dropped; stored throttle stays 0.
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, snapshotsFor(net, 0)[0].data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(snapshotsFor(net, 0)[0]);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // throttle=0 retained (idle thrust only) → vel stays below full-throttle level.
    CHECK(e.vel[0] < 0.2f);
}

TEST_CASE("WorldBroadcaster: onReceive discards stale seqNum (out-of-order)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // First packet: seqNum=5, throttle=1 accepted.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 5u;
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Out-of-order: seqNum=3 (stale) with throttle=0 must be dropped.
    inp.seqNum = 3u;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, snapshotsFor(net, 0)[0].data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(snapshotsFor(net, 0)[0]);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // throttle=1 retained → full thrust; vel clearly above zero (idle) level.
    CHECK(e.vel[0] > 0.05f);
}

TEST_CASE("WorldBroadcaster: onReceive accepts seqNum wrap-around", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;

    // Prime with UINT32_MAX; throttle=0 accepted (first packet, hasSeq=false).
    inp.seqNum = 0xFFFFFFFFu;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // seqNum=0 wraps around: isNewerSeq(0, UINT32_MAX) must return true.
    inp.seqNum = 0u;
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // seqNum=UINT32_MAX-1 is now stale (older than 0 under half-window); must be dropped.
    inp.seqNum = 0xFFFFFFFEu;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, snapshotsFor(net, 0)[0].data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(snapshotsFor(net, 0)[0]);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // seqNum=0 throttle=1 retained (UINT32_MAX-1 dropped) → full thrust; vel above zero (idle) level.
    CHECK(e.vel[0] > 0.05f);
}

TEST_CASE("WorldBroadcaster: onReceive computes estimatedDelayTicks from tickIndex", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Advance to tick 10 so m_currentTick = 10.
    broadcaster.onTick(1.0 / 60.0, 10u);
    clearSnapshots(net);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 1u;
    inp.tickIndex = 5u; // client last saw tick 5; delay = 10 - 5 = 5 ticks
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    uint32_t gotDelay = 0xFFFFFFFFu;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDelay = pi.delayTicks; });
    CHECK(gotDelay == 5u);
}

TEST_CASE("WorldBroadcaster: onReceive future tickIndex does not update estimatedDelayTicks", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Advance to tick 3 so m_currentTick = 3.
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);

    // Client sends tickIndex=10 (in the future from the server's perspective).
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 1u;
    inp.tickIndex = 10u; // tickIndex > m_currentTick: guard must prevent underflow
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    uint32_t gotDelay = 0xFFFFFFFFu;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDelay = pi.delayTicks; });
    // estimatedDelayTicks stays at its initialized value of 0 (no underflow).
    CHECK(gotDelay == 0u);
}

// ---------------------------------------------------------------------------
// Weather integration tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: with WeatherController broadcasts MsgWeatherState 0x04", "[world_broadcaster][weather]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WeatherController weather;
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, &weather);

    // Run 10 ticks — MsgWeatherState broadcasts every 10 ticks
    for (int i = 0; i < 10; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    // At least one broadcast should be MsgWeatherState (msgId == 0x04)
    bool foundWeather = false;
    for (const auto& pkt : net.broadcasts) {
        if (!pkt.empty() && pkt[0] == 0x04u) {
            foundWeather = true;
            // Verify minimum size
            CHECK(pkt.size() >= sizeof(fl::MsgWeatherState));
        }
    }
    CHECK(foundWeather);
}

TEST_CASE("WorldBroadcaster: without WeatherController does not broadcast 0x04", "[world_broadcaster][weather]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr);

    for (int i = 0; i < 10; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    for (const auto& pkt : net.broadcasts)
        if (!pkt.empty())
            CHECK(pkt[0] != 0x04u);
}

// ---------------------------------------------------------------------------
// Peer management: kick / ban / unban / forEachPeer
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: kickPeer calls disconnectPeer on network", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "1.2.3.4:5000";
    connectPilotPeer(broadcaster, net, 0u);
    net.disconnectedPeers.clear();

    broadcaster.kickPeer(0u);

    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: forEachPeer calls fn for each connected peer", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "1.2.3.4:5000";
    net.peerAddresses[1] = "5.6.7.8:6000";
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);

    int callCount = 0;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        fl::EntityId eid = pi.eid;
        CHECK(eid.valid());
        ++callCount;
    });
    CHECK(callCount == 2);
}

TEST_CASE("WorldBroadcaster: forEachPeer with no connected peers does not call fn", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    int callCount = 0;
    broadcaster.forEachPeer([&](const fl::PeerInfo&) { ++callCount; });
    CHECK(callCount == 0);
}

// ---------------------------------------------------------------------------
// Security: rate limiting
// ---------------------------------------------------------------------------

static std::vector<uint8_t> makeClientInputPacket() {
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.viewAxis[0] = 1.0f;
    std::vector<uint8_t> buf(sizeof(inp));
    std::memcpy(buf.data(), &inp, sizeof(inp));
    return buf;
}

// ---------------------------------------------------------------------------
// Security: allowlist
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Security: per-IP concurrent connection limit
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Connect handshake (#853)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Required-pack policy (#872 wire half, warn-only)
// ---------------------------------------------------------------------------

namespace {} // namespace

// ---------------------------------------------------------------------------
// Observer role (#857)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: a peer transitions pilot<->observer without reconnecting (#857)",
          "[world_broadcaster][observer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);
    REQUIRE(em.liveCount() == 1u);

    broadcaster.setPeerRole(0u, fl::PeerRole::Observer); // pilot -> observer despawns the entity
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK(em.liveCount() == 0u);

    broadcaster.setPeerRole(0u, fl::PeerRole::Pilot); // observer -> pilot respawns
    broadcaster.onTick(1.0 / 60.0, 3u);
    CHECK(em.liveCount() == 1u);
}

// ---------------------------------------------------------------------------
// Security: flood detection
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: peer within flood limit is not disconnected", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120 packets/s

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);

    auto pkt = makeClientInputPacket();
    for (int i = 0; i < 120; ++i) // exactly at threshold: not over
        broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: peer exceeding flood limit is disconnected", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120 packets/s

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);

    auto pkt = makeClientInputPacket();
    for (int i = 0; i < 121; ++i) // 121 > 120: over threshold
        broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.disconnectedPeers.size() >= 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: flood counter resets after 1s window", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);

    auto pkt = makeClientInputPacket();
    // Send 120 (at limit)
    for (int i = 0; i < 120; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());

    // Advance past the 1s window, counter resets
    t.advance(std::chrono::seconds(2));
    // Send 120 more — still at limit in the new window
    for (int i = 0; i < 120; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: non-ClientInput packets do not count toward flood", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);

    // Send 500 packets with an unknown msgId — should not trigger flood
    uint8_t unknownMsg = 0xFF;
    for (int i = 0; i < 500; ++i)
        broadcaster.onReceive(0u, &unknownMsg, 1u);

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: onDisconnect clears flood state", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 1); // threshold = 60

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);

    // Fill flood state to 59 packets (just under threshold)
    auto pkt = makeClientInputPacket();
    for (int i = 0; i < 59; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());

    // Disconnect + reconnect — flood state should be cleared
    broadcaster.onDisconnect(0u);
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 0u);

    // Should be able to send 60 packets in the new window without triggering flood
    for (int i = 0; i < 60; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// Security: ban set management
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Security: edge cases for branch coverage
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Shutdown countdown tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: initiateShutdown broadcasts first notice on next tick", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5);

    CHECK(broadcaster.isShuttingDown());
    CHECK(broadcaster.secondsUntilShutdown() <= 30u);
    // Cross-thread beacon mirror (#226).
    CHECK(broadcaster.getShutdownStatus().active);
    CHECK(broadcaster.getShutdownStatus().secondsRemaining == 30u);
    broadcaster.cancelShutdown();
    CHECK_FALSE(broadcaster.getShutdownStatus().active);
    broadcaster.initiateShutdown(30, 5); // restore for the rest of the test

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining > 0u);
    CHECK(notice.secondsRemaining <= 30u);
    CHECK(notice.text[0] != '\0');
}

TEST_CASE("WorldBroadcaster: no notice broadcast when interval not reached", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(60, 30); // 30s interval

    // First tick fires first notice.
    broadcaster.onTick(1.0 / 60.0, 0u);
    std::size_t noticesAfterFirst = 0;
    for (const auto& pkt : net.broadcasts)
        if (pkt.size() == sizeof(fl::MsgServerNotice) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
            ++noticesAfterFirst;
    REQUIRE(noticesAfterFirst == 1u);

    // Advance only 10s (halfway through 30s interval) — no new notice expected.
    t.advance(std::chrono::seconds(10));
    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    std::size_t newNotices = 0;
    for (const auto& pkt : net.broadcasts)
        if (pkt.size() == sizeof(fl::MsgServerNotice) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
            ++newNotices;
    CHECK(newNotices == 0u);
}

TEST_CASE("WorldBroadcaster: cancelShutdown stops countdown", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(60, 30);
    broadcaster.onTick(1.0 / 60.0, 0u);
    broadcaster.cancelShutdown();
    CHECK(!broadcaster.isShuttingDown());

    net.broadcasts.clear();
    t.advance(std::chrono::seconds(70)); // past original shutdown time
    broadcaster.onTick(1.0 / 60.0, 1u);

    fl::MsgServerNotice notice{};
    CHECK(!findNotice(net.broadcasts, 0, notice));
}

TEST_CASE("WorldBroadcaster: extendShutdown pushes back and fires immediate notice", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5);
    broadcaster.onTick(1.0 / 60.0, 0u);

    t.advance(std::chrono::seconds(20));
    net.broadcasts.clear();

    REQUIRE(broadcaster.extendShutdown(60u));

    broadcaster.onTick(1.0 / 60.0, 1u);
    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining > 50u); // ~70s remaining after extension
}

TEST_CASE("WorldBroadcaster: extendShutdown returns false when not shutting down", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    CHECK(!broadcaster.extendShutdown(60u));
}

TEST_CASE("WorldBroadcaster: shutdown callback fires at T=0", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    bool called = false;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.match.shutdown = [&called]() { called = true; };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(10, 5);

    t.advance(std::chrono::seconds(11));
    broadcaster.onTick(1.0 / 60.0, 0u);

    CHECK(called);
    CHECK(!broadcaster.isShuttingDown());
    CHECK(broadcaster.secondsUntilShutdown() == 0u);
}

TEST_CASE("WorldBroadcaster: T=0 fires without crash when no callback set", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(0, 0);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining == 0u);
    CHECK(!broadcaster.isShuttingDown());
}

TEST_CASE("WorldBroadcaster: initiateShutdown delay=0 fires on very next tick", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    bool called = false;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.match.shutdown = [&called]() { called = true; };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(0, 0);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining == 0u);
    CHECK(called);
}

TEST_CASE("WorldBroadcaster: T-60 notice always fires with 5-min interval", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(600, 300); // 10 min, 5-min interval

    broadcaster.onTick(1.0 / 60.0, 0u); // first notice at T-600s

    // Advance to T-61s (would skip T-60s without the clamp logic).
    t.advance(std::chrono::seconds(539));
    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining <= 62u);
    CHECK(notice.secondsRemaining >= 58u);
}

TEST_CASE("WorldBroadcaster: notice text contains hours for delays >= 3600s", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(7200, 3600);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("hour") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: notice text contains minutes for delays 61-3599s", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(300, 60);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("minute") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: notice text uses final-minute wording at T-60s", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(300, 60);

    t.advance(std::chrono::seconds(245)); // T-55s remaining
    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("1 minute") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: initiateShutdown with reason includes reason in countdown notice",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(300, 5, "Server restarting");

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    std::string text(notice.text);
    CHECK(text.find("Server restarting") != std::string::npos);
    CHECK(text.find("minutes") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: initiateShutdown with reason uses short format for secsLeft at most 60",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5, "Server restarting");

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    std::string text(notice.text);
    CHECK(text.find("Server restarting") != std::string::npos);
    CHECK(text.find("1 minute") != std::string::npos);
    CHECK(text.find("save your progress") == std::string::npos);
}

TEST_CASE("WorldBroadcaster: initiateShutdown with reason includes reason in T=0 notice",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(0, 5, "Server restarting");

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining == 0u);
    CHECK(std::string(notice.text).find("Server restarting") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: long reason is safely truncated to fit MsgServerNotice text",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    std::string longReason(60, 'X');
    broadcaster.initiateShutdown(300, 5, longReason);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::strlen(notice.text) < sizeof(notice.text));
}

TEST_CASE("WorldBroadcaster: cancelShutdown clears reason so subsequent shutdown uses default text",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5, "reason text");
    broadcaster.cancelShutdown();
    broadcaster.initiateShutdown(30, 5);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    std::string text(notice.text);
    CHECK(text.find("reason text") == std::string::npos);
    CHECK(text.find("Server shutting down") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: extendShutdown preserves reason in subsequent notices", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(120, 60, "Server restarting");

    // First tick fires first notice.
    broadcaster.onTick(1.0 / 60.0, 0u);
    std::size_t firstNoticeIdx = net.broadcasts.size() - 1;

    // Advance 60s to reach next notice interval then extend.
    t.advance(std::chrono::seconds(60));
    broadcaster.extendShutdown(60);
    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("Server restarting") != std::string::npos);
    (void)firstNoticeIdx;
}

// ---------------------------------------------------------------------------
// MsgAdminCommand / MsgAdminResponse tests
// ---------------------------------------------------------------------------

namespace {

// Build a MsgAdminCommand packet with the given token and command strings.
static std::vector<uint8_t> makeAdminCmd(const char* token, const char* command, uint16_t reqId = 0x0042u) {
    fl::MsgAdminCommand msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    msg.reqId = reqId;
    std::snprintf(msg.token, sizeof(msg.token), "%s", token);
    std::snprintf(msg.command, sizeof(msg.command), "%s", command);
    return {reinterpret_cast<const uint8_t*>(&msg), reinterpret_cast<const uint8_t*>(&msg) + sizeof(msg)};
}

// Minimal mock for CommandShell mark/drainSince injection. Pre-load `lines` before
// advancing the tick; drainSince returns whatever is in `lines` regardless of the mark.
struct ShellDrainMock {
    std::vector<std::string> lines;
    int mark() const {
        return 0;
    }
    std::vector<std::string> drainSince(int /*mark*/) const {
        return lines;
    }
};

// The ENet frontend's AdminChannel, built and attached in one line (#1079). It replaces what used to
// be three separate broadcaster setters -- setAdminDispatch, setAdminShell and setAdminAuthParams --
// and holds the auth state those tests inspect.
struct TestAdminChannel {
    fl::AdminChannel ch;

    // Registers itself into the hooks struct, which is then handed to the constructor: the channel is
    // frozen at construction with everything else now (#1082), so it is declared BEFORE the broadcaster.
    TestAdminChannel(fl::WorldBroadcasterHooks& hooks, fl::AdminChannel::Dispatcher fn,
                     const fl::IClock& clock = fl::SystemClock::instance(), int maxFailures = 5,
                     int lockoutSeconds = 300)
        : ch(std::move(fn), makeCfg(maxFailures, lockoutSeconds), clock) {
        hooks.comms.adminChannel = &ch;
    }

    void attachShell(ShellDrainMock& shell) {
        ch.setShellTap([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });
    }

  private:
    static fl::AdminChannel::Config makeCfg(int maxFailures, int lockoutSeconds) {
        fl::AdminChannel::Config c;
        c.name = "enet";
        c.maxAuthFailures = maxFailures;
        c.lockoutSeconds = lockoutSeconds;
        return c;
    }
};

// Collect all MsgAdminResponse and MsgAdminResponseChunk packets from net.sends that
// arrive at or after index `afterIdx`, filtered by msgId. Used to isolate deferred drain
// output from the synchronous ack and WorldSnapshot sends that onTick also emits.
static std::vector<std::vector<uint8_t>> drainSends(const MockNetwork& net, std::size_t afterIdx) {
    std::vector<std::vector<uint8_t>> result;
    for (std::size_t i = afterIdx; i < net.sends.size(); ++i) {
        const auto& pkt = net.sends[i];
        if (pkt.empty())
            continue;
        uint8_t id = pkt[0];
        if (id == static_cast<uint8_t>(fl::MsgId::AdminResponse) ||
            id == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk))
            result.push_back(pkt);
    }
    return result;
}

// Return true if the last entry in net.sends is a MsgAdminResponseChunk; populate chunk.
static bool parseLastChunk(const MockNetwork& net, fl::MsgAdminResponseChunk& chunk) {
    if (net.sends.empty())
        return false;
    const auto& last = net.sends.back();
    if (last.size() != sizeof(fl::MsgAdminResponseChunk))
        return false;
    std::memcpy(&chunk, last.data(), sizeof(chunk));
    return chunk.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk);
}

// ---------------------------------------------------------------------------
// MOTD helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Admin command helpers (existing)
// ---------------------------------------------------------------------------

// Return true if the last entry in net.sends is a MsgAdminResponse; populate resp.
static bool parseLastAdminResponse(const MockNetwork& net, fl::MsgAdminResponse& resp) {
    if (net.sends.empty())
        return false;
    const auto& last = net.sends.back();
    if (last.size() != sizeof(fl::MsgAdminResponse))
        return false;
    std::memcpy(&resp, last.data(), sizeof(resp));
    return resp.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponse);
}

} // namespace

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded when no dispatcher set", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret"); // dispatcher NOT set

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded when no password configured",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string {
        return "pong";
    }); // password NOT set
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded on wrong token", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "pong"; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("wrongpass", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand dispatches on correct token and sends MsgAdminResponse",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [](std::string_view cmd, const CommandIssuer&) -> std::string {
        if (cmd == "ping")
            return "pong";
        return "";
    });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "ping");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::string(resp.text) == "pong");
    CHECK(net.sendReliable);
}

// --- Grant channel (#946): empty-token MsgAdminCommand authenticated by granted caps ---

TEST_CASE("WorldBroadcaster: empty-token admin command dispatches with granted caps (#946)",
          "[world_broadcaster][admin_command][permission]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::CapabilityMask seenCaps = 0;
    uint32_t seenPeer = 0xFFFFFFFFu;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [&](std::string_view, const CommandIssuer& iss) -> std::string {
        seenCaps = iss.caps;
        seenPeer = iss.peerId;
        return "ok";
    });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    // Grant the peer game-master caps; it now authenticates by caps, not the operator password.
    REQUIRE(broadcaster.setPeerAuthority(0u, fl::PeerAuthority{fl::kGameMasterCaps, 5}));
    net.sends.clear();

    auto pkt = makeAdminCmd("", "spawn foo 0 0 0"); // empty token -> grant channel
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(seenCaps == fl::kGameMasterCaps); // the dispatcher saw the granted caps, not Admin
    CHECK(seenPeer == 0u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::string(resp.text) == "ok");
}

// Scan sends for a ConnectAck packet and parse its granted-authority TLV (#949). Returns true if a
// ConnectAck carrying ConnectAckAuthority was found, filling caps/faction.
static bool findConnectAckAuthority(const MockNetwork& net, uint64_t& caps, uint16_t& faction) {
    for (const auto& pkt : net.sends) {
        if (pkt.size() < sizeof(fl::MsgConnectAck) || pkt[0] != static_cast<uint8_t>(fl::MsgId::ConnectAck))
            continue;
        fl::MsgConnectAck ack{};
        std::memcpy(&ack, pkt.data(), sizeof(ack));
        const std::size_t off = sizeof(fl::MsgConnectAck) + std::size_t(ack.typeCount) * sizeof(fl::MsgEntityTypeDef);
        if (off > pkt.size())
            continue;
        uint16_t valueLen = 0;
        const uint8_t* p = fl::findExt(pkt.data() + off, pkt.size() - off,
                                       static_cast<uint16_t>(fl::ExtTag::ConnectAckAuthority), valueLen);
        if (p && valueLen >= sizeof(uint64_t) + sizeof(uint16_t)) {
            std::memcpy(&caps, p, sizeof(caps));
            std::memcpy(&faction, p + sizeof(caps), sizeof(faction));
            return true;
        }
    }
    return false;
}

TEST_CASE("WorldBroadcaster: grant re-sends ConnectAck with the authority TLV (#949)",
          "[world_broadcaster][admin_command][permission]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    // A grant re-sends ConnectAck carrying the TLV so the client can show GM UI.
    REQUIRE(broadcaster.setPeerAuthority(0u, fl::PeerAuthority{fl::kGameMasterCaps, 4}));
    uint64_t caps = 0;
    uint16_t faction = 0xFFFFu;
    REQUIRE(findConnectAckAuthority(net, caps, faction));
    CHECK(caps == fl::kGameMasterCaps);
    CHECK(faction == 4u);

    // A revoke re-sends ConnectAck with NO TLV (caps back to zero).
    net.sends.clear();
    REQUIRE(broadcaster.setPeerAuthority(0u, fl::PeerAuthority{}));
    uint64_t caps2 = 0;
    uint16_t faction2 = 0;
    CHECK_FALSE(findConnectAckAuthority(net, caps2, faction2)); // no TLV on the revoke ack
    // But a ConnectAck WAS re-sent (so the client re-parses and clears its caps).
    bool sawAck = false;
    for (const auto& pkt : net.sends)
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            sawAck = true;
    CHECK(sawAck);
}

// Count GmWorldState packets sent to a peer, and total records across them.
static void gmFeedFor(const MockNetwork& net, uint32_t peerId, std::size_t& packets, std::size_t& records) {
    packets = 0;
    records = 0;
    for (const auto& [pid, pkt] : net.perPeerSends) {
        if (pid != peerId || pkt.empty() || pkt[0] != static_cast<uint8_t>(fl::MsgId::GmWorldState))
            continue;
        REQUIRE(pkt.size() >= sizeof(fl::MsgGmWorldStateHeader));
        fl::MsgGmWorldStateHeader hdr{};
        std::memcpy(&hdr, pkt.data(), sizeof(hdr));
        ++packets;
        records += hdr.count;
        CHECK(pkt.size() == sizeof(fl::MsgGmWorldStateHeader) + std::size_t(hdr.count) * sizeof(fl::GmEntityRecord));
    }
}

TEST_CASE("WorldBroadcaster: GM world-state feed goes only to GmMap-capable peers (#861)",
          "[world_broadcaster][gm_map]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.0.0.1:1";
    net.peerAddresses[1] = "1.0.0.2:2";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    for (int i = 0; i < 5; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = static_cast<double>(i * 100);
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    connectPilotPeer(broadcaster, net, 0u); // GM peer
    connectPilotPeer(broadcaster, net, 1u); // ordinary peer
    REQUIRE(broadcaster.setPeerAuthority(0u, fl::PeerAuthority{fl::kGameMasterCaps, 0xFFFFu}));

    net.perPeerSends.clear();
    broadcaster.onTick(1.0 / 60.0, 0u); // rebuild + GM broadcast tick

    std::size_t gmPackets = 0, gmRecords = 0;
    gmFeedFor(net, 0u, gmPackets, gmRecords);
    CHECK(gmPackets >= 1u);
    CHECK(gmRecords == 7u); // 5 spawned + 2 pilots

    std::size_t otherPackets = 0, otherRecords = 0;
    gmFeedFor(net, 1u, otherPackets, otherRecords);
    CHECK(otherPackets == 0u); // the non-GM peer gets no feed
}

TEST_CASE("WorldBroadcaster: GM feed chunks large entity sets under the MTU (#861)", "[world_broadcaster][gm_map]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.0.0.1:1";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    // More than kMaxGmRecordsPerPacket entities -> multiple chunks.
    const int kSpawn = static_cast<int>(fl::kMaxGmRecordsPerPacket) * 2 + 3;
    for (int i = 0; i < kSpawn; ++i) {
        fl::EntityTransform t{};
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    connectPilotPeer(broadcaster, net, 0u);
    REQUIRE(broadcaster.setPeerAuthority(0u, fl::PeerAuthority{fl::kGameMasterCaps, 0xFFFFu}));

    net.perPeerSends.clear();
    broadcaster.onTick(1.0 / 60.0, 0u);

    std::size_t packets = 0, records = 0;
    gmFeedFor(net, 0u, packets, records);
    CHECK(packets >= 3u);                       // 25 + 25 + rest
    CHECK(records == std::size_t(kSpawn) + 1u); // + the pilot entity
}

TEST_CASE("WorldBroadcaster: worldState aggregate is rebuilt at ~1 Hz and lists live entities (#600)",
          "[world_broadcaster][world_state]") {
    MockLogger log;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    for (int i = 0; i < 3; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = static_cast<double>(i * 10);
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    connectPilotPeer(broadcaster, net, 0u); // adds a 4th entity (the pilot)

    // Tick 0 is a rebuild tick (0 % 60 == 0): the aggregate reflects the live set.
    broadcaster.onTick(1.0 / 60.0, 0u);
    const fl::WorldStateSnapshot& ws = broadcaster.worldState();
    CHECK(ws.tick == 0u);
    CHECK(ws.entities.size() == 4u); // 3 spawned + 1 pilot
    CHECK(ws.peers.size() == 1u);    // the connected pilot appears in the peer picture
    // Entities are in ascending pool order.
    for (std::size_t i = 1; i < ws.entities.size(); ++i)
        CHECK(ws.entities[i - 1].entityIdx < ws.entities[i].entityIdx);

    // A non-multiple-of-60 tick does not rebuild (the snapshot stays at tick 0).
    broadcaster.onTick(1.0 / 60.0, 30u);
    CHECK(broadcaster.worldState().tick == 0u);
    // The next multiple advances it.
    broadcaster.onTick(1.0 / 60.0, 60u);
    CHECK(broadcaster.worldState().tick == 60u);
}

TEST_CASE("WorldBroadcaster: worldState reports wing sweep for a variable-geometry aircraft (#1195)",
          "[world_broadcaster][world_state][sweep]") {
    // The headless route. Sweep lives in the flight integrator and nothing outside it could read the
    // angle — not the Lua state table, not --mission-report, not the replay, not any console
    // command — so a swing-wing aircraft's own acceptance criterion ("sweep follows the Mach
    // schedule in telemetry") could not be evaluated at all. It is on the ~1 Hz aggregate now, which
    // the `worldstate` command, GET /worldstate and the MCP resource all serve.
    MockLogger log;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityDef def = makeDebugDef();
    def.flightModelAsset = "models/vg";
    registry.registerType(def);
    fl::EntityManager em(log, registry);

    // ref_sweep_deg 33, range 15..67.5, schedule flat at 33 — so a parked aircraft is at a sweep
    // that is neither limit nor FlightState's bare 55 deg default, which is exactly what a mistake
    // here would report.
    const auto vg = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(makeVgFlightModelToml(33.f)));
    REQUIRE(vg->wing_sweep.has_value());
    fl::WorldQueries queries;
    queries.flightModel = [&](const std::string&) { return vg; };
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, std::move(queries));
    connectPilotPeer(broadcaster, net, 0u);

    broadcaster.onTick(1.0 / 60.0, 0u);
    const fl::WorldStateSnapshot& ws = broadcaster.worldState();
    REQUIRE(ws.entities.size() == 1u);
    CHECK(ws.entities[0].sweepDeg == Catch::Approx(33.f).margin(0.5f));
}

TEST_CASE("WorldBroadcaster: worldState reports zero sweep for a fixed-geometry aircraft (#1195)",
          "[world_broadcaster][world_state][sweep]") {
    // FlightState's default is a bare 55 deg and the spawn path used to hand it straight to the
    // integrator, so every fixed-geometry aircraft in the game carried a wing-sweep angle it does
    // not have. Harmless while nothing read it; a wrong number the moment it became telemetry.
    MockLogger log;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef()); // the builtin model — no [wing_sweep]
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    connectPilotPeer(broadcaster, net, 0u);

    broadcaster.onTick(1.0 / 60.0, 0u);
    REQUIRE(broadcaster.worldState().entities.size() == 1u);
    CHECK(broadcaster.worldState().entities[0].sweepDeg == 0.f);
}

TEST_CASE("WorldBroadcaster: password auth grants Admin caps (rung 1 unchanged, #946)",
          "[world_broadcaster][admin_command][permission]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::CapabilityMask seenCaps = 0;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [&](std::string_view, const CommandIssuer& iss) -> std::string {
        seenCaps = iss.caps;
        return "ok";
    });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(seenCaps == fl::kAdminCaps); // password path arrives with full authority
}

TEST_CASE("WorldBroadcaster: zero-cap empty-token command refused without lockout pollution (#947)",
          "[world_broadcaster][admin_command][permission]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    bool dispatched = false;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster,
        [&](std::string_view, const CommandIssuer&) -> std::string {
            dispatched = true;
            return "ok";
        },
        fl::SystemClock::instance(), /*maxFailures=*/3, /*lockoutSeconds=*/60); // 3 genuine failures = lockout
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));

    connectPilotPeer(broadcaster, net, 0u); // peer holds zero caps
    net.sends.clear();
    broadcaster.setOperatorPassword("secret");

    // Several empty-token commands: refused every time, never dispatched, and — critically — never
    // an auth failure, so the peer is never locked out or kicked (a permission refusal is not a
    // brute-force attempt).
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK_FALSE(dispatched);
    CHECK(net.disconnectedPeers.empty());
    CHECK(admin.ch.authSummary().activeCount == 0);
    // The refusal is surfaced to the client (a password is configured, so the channel is "on").
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::string(resp.text).find("permission denied") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded if packet too small", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "pong"; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    // Send only the msgId byte + 3 padding — well under sizeof(MsgAdminCommand).
    uint8_t tiny[4] = {static_cast<uint8_t>(fl::MsgId::AdminCommand), 0, 0, 0};
    broadcaster.onReceive(0u, tiny, sizeof(tiny));

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand token without null terminator fails auth",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "pong"; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    // Fill entire token field with 'x' (no null byte) — auth must fail, no crash.
    fl::MsgAdminCommand msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    std::memset(msg.token, 'x', sizeof(msg.token));
    std::snprintf(msg.command, sizeof(msg.command), "ping");
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&msg),
                                    reinterpret_cast<const uint8_t*>(&msg) + sizeof(msg));
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand with empty command string is discarded",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "should not be called"; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    // Valid token, but command field is all zeros (empty after null-term).
    fl::MsgAdminCommand msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    std::snprintf(msg.token, sizeof(msg.token), "secret");
    // command left zero-initialized — empty string
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&msg),
                                    reinterpret_cast<const uint8_t*>(&msg) + sizeof(msg));
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand empty dispatcher result still sends MsgAdminResponse",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return ""; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");
    // Dispatcher returns empty string (e.g. fire-and-forget command).

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    // Server always sends a response; client-side filters empty text before printing.
    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(resp.text[0] == '\0');
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand result >123 chars streams as MsgAdminResponseChunk",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster,
                           [](std::string_view, const CommandIssuer&) -> std::string { return std::string(200, 'x'); });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponseChunk chunk{};
    REQUIRE(parseLastChunk(net, chunk));
    CHECK((chunk.flags & fl::kChunkFlagEnd) != 0u);
    CHECK(std::strlen(chunk.body) == 200u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse fast-path for result <=123 chars",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster,
                           [](std::string_view, const CommandIssuer&) -> std::string { return std::string(50, 'a'); });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::strlen(resp.text) == 50u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse fast-path at exactly 123 chars", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster,
                           [](std::string_view, const CommandIssuer&) -> std::string { return std::string(123, 'x'); });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "help");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::strlen(resp.text) == 123u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse echoes reqId in MsgAdminResponse",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "ok"; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "ping", 0xBEEFu);
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(resp.reqId == 0xBEEFu);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse 124-char result sends one chunk with kChunkFlagEnd",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster,
                           [](std::string_view, const CommandIssuer&) -> std::string { return std::string(124, 'y'); });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "help");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponseChunk chunk{};
    REQUIRE(parseLastChunk(net, chunk));
    CHECK(chunk.seqNum == 0u);
    CHECK((chunk.flags & fl::kChunkFlagEnd) != 0u);
    CHECK(std::strlen(chunk.body) == 124u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse >505 chars sends two chunks", "[world_broadcaster][admin_command]") {
    const std::string longResult(506, 'z');

    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster,
                           [&](std::string_view, const CommandIssuer&) -> std::string { return longResult; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 2u);

    fl::MsgAdminResponseChunk c0{}, c1{};
    std::memcpy(&c0, net.sends[0].data(), sizeof(c0));
    std::memcpy(&c1, net.sends[1].data(), sizeof(c1));

    CHECK(c0.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk));
    CHECK(c0.seqNum == 0u);
    CHECK((c0.flags & fl::kChunkFlagEnd) == 0u); // not the final chunk
    CHECK(c1.seqNum == 1u);
    CHECK((c1.flags & fl::kChunkFlagEnd) != 0u); // final chunk

    std::string assembled = std::string(c0.body) + std::string(c1.body);
    CHECK(assembled == longResult);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse echoes reqId in every MsgAdminResponseChunk",
          "[world_broadcaster][admin_command]") {
    const std::string longResult(506, 'q');

    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(h_broadcaster,
                           [&](std::string_view, const CommandIssuer&) -> std::string { return longResult; });
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setOperatorPassword("secret");

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "peers", 0x1111u);
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 2u);
    for (const auto& send : net.sends) {
        REQUIRE(send.size() == sizeof(fl::MsgAdminResponseChunk));
        fl::MsgAdminResponseChunk chunk{};
        std::memcpy(&chunk, send.data(), sizeof(chunk));
        CHECK(chunk.reqId == 0x1111u);
    }
}

// ---------------------------------------------------------------------------
// Admin auth lockout tests
// ---------------------------------------------------------------------------

// Shared setup helper: broadcaster with password "pw", dispatch noop, 3-failure threshold, 60 s
// lockout on the injected clock, and peer 0 connected from 1.2.3.4. Returns the channel because the
// broadcaster holds a pointer to it -- the caller has to outlive the broadcaster's use of it.
// The channel these tests share. Built BEFORE the broadcaster and handed to it through the hooks
// struct, because an admin channel is frozen at construction now (#1082).
static std::unique_ptr<TestAdminChannel> makeAuthChannel(fl::WorldBroadcasterHooks& hooks, fl::ManualClock& now) {
    return std::make_unique<TestAdminChannel>(
        hooks, [](std::string_view, const CommandIssuer&) -> std::string { return "ok"; }, now,
        /*maxFailures=*/3, /*lockoutSeconds=*/60);
}

// The rest of the old setupAuthFixture, run once the broadcaster exists.
static void startAuthFixture(fl::WorldBroadcaster& broadcaster, MockNetwork& net, fl::ManualClock& now) {
    broadcaster.setOperatorPassword("pw");
    broadcaster.setClock(now);
    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();
    net.disconnectedPeers.clear();
}

TEST_CASE("WorldBroadcaster: admin auth no lockout before threshold", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // N-1 = 2 failures; peer must remain connected after each
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(net.disconnectedPeers.empty());
    }
}

TEST_CASE("WorldBroadcaster: admin auth lockout triggered on Nth failure -- peer kicked",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // 2 failures — no kick
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // 3rd failure — lockout: peer kicked
    auto pkt = makeAdminCmd("wrongpass", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: admin auth onConnect refused while locked", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678"; // same IP, new port (reconnect)
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // Trigger lockout on peer 0
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Reconnect attempt from same IP — must be refused
    connectPilotPeer(broadcaster, net, 1u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 1u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Access denied.");
}

TEST_CASE("WorldBroadcaster: admin auth lockout expires after TTL", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // Trigger lockout
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past 60 s TTL
    now.advance(std::chrono::seconds(61));

    // Reconnect — should succeed (MsgHello sent, not in disconnectedPeers)
    connectPilotPeer(broadcaster, net, 1u);
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(!net.sends.empty());
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends.front().data(), sizeof(hello));
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
}

TEST_CASE("WorldBroadcaster: admin auth per-IP isolation", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234"; // IP A
    net.peerAddresses[1] = "5.6.7.8:2222"; // IP B — different
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // Connect peer 1 from IP B
    connectPilotPeer(broadcaster, net, 1u);
    net.sends.clear();
    net.disconnectedPeers.clear();

    // Lock out IP A (3 wrong tokens on peer 0)
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // IP B can still dispatch successfully
    auto pkt = makeAdminCmd("pw", "status");
    broadcaster.onReceive(1u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::string(resp.text) == "ok");
}

TEST_CASE("WorldBroadcaster: admin auth failure counter persists across disconnect-reconnect",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678"; // same normalized IP, new peerId
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // 2 failures on peer 0 — below threshold
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // Peer 0 disconnects
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Peer 1 reconnects from same IP; one more failure should trigger lockout (counter IP-keyed)
    connectPilotPeer(broadcaster, net, 1u);
    net.sends.clear();
    net.disconnectedPeers.clear();

    auto pkt = makeAdminCmd("wrongpass", "status");
    broadcaster.onReceive(1u, pkt.data(), pkt.size());
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 1u);
}

TEST_CASE("WorldBroadcaster: admin auth correct token resets failure counter", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // N-1 = 2 failures
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // Successful auth — clears the counter
    auto good = makeAdminCmd("pw", "status");
    broadcaster.onReceive(0u, good.data(), good.size());
    CHECK(net.disconnectedPeers.empty());

    // 2 more failures — should NOT trigger lockout (counter was reset)
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: admin auth wrong tokens when operator_password unset do not record failures",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    broadcaster.setClock(now);
    // Note: neither an operator password nor an AdminChannel — the frontend is off
    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();
    net.disconnectedPeers.clear();

    // Send N+5 = 8 wrong-token packets — admin channel is disabled so none are processed
    for (int i = 0; i < 8; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // Reconnect from same IP — must not be blocked (no failures recorded)
    connectPilotPeer(broadcaster, net, 1u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: admin auth pruneExpired fires after 600 onTick calls",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // Trigger lockout
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance past TTL — lockout entry is now expired but not yet pruned
    now.advance(std::chrono::seconds(61));

    // Drive 600 onTick calls to trigger the prune cycle
    for (int i = 0; i < 600; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    // Clear sends accumulated during 600 ticks (per-peer unicast snapshots from any still-connected
    // peers), then reconnect and verify MsgHello is the first new send.
    net.sends.clear();
    net.perPeerSends.clear();

    // After prune + TTL expiry, reconnect from same IP must succeed
    connectPilotPeer(broadcaster, net, 1u);
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(!net.sends.empty());
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends.front().data(), sizeof(hello));
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
}

TEST_CASE("WorldBroadcaster: admin_unlock clears lockout -- onConnect succeeds", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // Trigger lockout on peer 0
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Unlock: should report that the lockout was active
    CHECK(admin->ch.clearLockout("1.2.3.4"));

    // Reconnect from same IP must now succeed
    connectPilotPeer(broadcaster, net, 1u);
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(!net.sends.empty());
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends.front().data(), sizeof(hello));
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
}

TEST_CASE("WorldBroadcaster: admin_unlock is a no-op when IP is not locked", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock now;
    fl::WorldBroadcasterHooks h_broadcaster;
    auto admin = makeAuthChannel(h_broadcaster, now);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    startAuthFixture(broadcaster, net, now);

    // No failures — clearing must report the IP was not locked
    CHECK_FALSE(admin->ch.clearLockout("1.2.3.4"));

    // Connect peer 1 from same IP: must not be refused
    connectPilotPeer(broadcaster, net, 1u);
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// Admin shell drain tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: admin shell drain sends no follow-on when shell not configured",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    // no shell tap on the channel — this frontend has no drain

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size(); // 1 sync ack

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain does not fire before wall-clock deadline",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size(); // 1 sync ack only

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Drain does not fire before the 20 ms deadline (clock not yet advanced).
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain fires after wall-clock deadline and forwards shell lines",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn", 0x0001u);
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    // Simulate callback output becoming available.
    shell.lines.push_back("[admin] spawned builtin:debug-entity entity=1/1");

    // Advance clock past 20 ms deadline — drain fires.
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&resp, drain[0].data(), sizeof(resp));
    CHECK(std::string(resp.text) == "[admin] spawned builtin:debug-entity entity=1/1");
}

TEST_CASE("WorldBroadcaster: admin shell drain sends nothing when drain returns empty vector",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell; // lines stays empty
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "set_weather");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain skips disconnected peer", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Peer disconnects before the drain deadline.
    broadcaster.onDisconnect(0u);

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain echoes correct reqId in follow-on response",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn", 0xABCDu);
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&resp, drain[0].data(), sizeof(resp));
    CHECK(resp.reqId == 0xABCDu);
}

TEST_CASE("WorldBroadcaster: two admin commands queue independent drains with separate reqIds",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd1 = makeAdminCmd("secret", "spawn", 0x0001u);
    auto cmd2 = makeAdminCmd("secret", "kill", 0x0002u);
    broadcaster.onReceive(0u, cmd1.data(), cmd1.size());
    broadcaster.onReceive(0u, cmd2.data(), cmd2.size());
    std::size_t sendsAfterRecv = net.sends.size(); // 2 sync acks

    // Both drains return the same lines (mark/drainSince mock ignores the mark value).
    shell.lines.push_back("[admin] result line");

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Two deferred responses, one per pending drain.
    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 2u);

    // Extract reqIds from both responses.
    fl::MsgAdminResponse r0{}, r1{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    REQUIRE(drain[1].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&r0, drain[0].data(), sizeof(r0));
    std::memcpy(&r1, drain[1].data(), sizeof(r1));

    std::vector<uint16_t> reqIds{r0.reqId, r1.reqId};
    std::sort(reqIds.begin(), reqIds.end());
    CHECK(reqIds[0] == 0x0001u);
    CHECK(reqIds[1] == 0x0002u);
}

TEST_CASE("WorldBroadcaster: admin shell drain fires exactly once", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Advance past deadline: drain fires on tick 1.
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);
    std::size_t sendsAfterTick1 = net.sends.size();

    // Tick 2: drain must NOT fire again (entry was erased).
    broadcaster.onTick(1.0 / 60.0, 2u);

    CHECK(drainSends(net, sendsAfterTick1).empty());
    // And tick 1 did deliver exactly one drain response.
    CHECK(drainSends(net, sendsAfterRecv).size() == 1u);
}

TEST_CASE("WorldBroadcaster: admin shell drain with long output streams as MsgAdminResponseChunk",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    // Line longer than kAdminResponseFastPathMax (123) → must route via chunk stream.
    shell.lines.push_back(std::string(200, 'x'));

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponseChunk chunk{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponseChunk));
    std::memcpy(&chunk, drain[0].data(), sizeof(chunk));
    CHECK(chunk.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk));
    CHECK((chunk.flags & fl::kChunkFlagEnd) != 0u);
    CHECK(std::strlen(chunk.body) == 200u);
}

TEST_CASE("WorldBroadcaster: admin shell drain joins multiple lines with newline",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines = {"line1", "line2", "line3"};

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&resp, drain[0].data(), sizeof(resp));
    CHECK(std::string(resp.text) == "line1\nline2\nline3");
}

TEST_CASE("WorldBroadcaster: admin shell drain sends nothing when all drain lines are empty",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    // All-empty strings: join produces "\n", pop_back gives "", guard suppresses send.
    shell.lines = {"", ""};

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain fires at wall-clock deadline regardless of tick index",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Three ticks without advancing the clock: drain must NOT fire on any of them
    // (simulates GameLoop catch-up where tick N+1 fires before callbacks from tick N run).
    broadcaster.onTick(1.0 / 60.0, 1u);
    broadcaster.onTick(1.0 / 60.0, 2u);
    broadcaster.onTick(1.0 / 60.0, 3u);
    CHECK(drainSends(net, sendsAfterRecv).empty());

    // Advance clock past the 20 ms deadline; drain fires on next onTick.
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 4u);
    CHECK(drainSends(net, sendsAfterRecv).size() == 1u);
}

TEST_CASE("WorldBroadcaster: two admin commands at staggered deadlines drain independently",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::ManualClock t;
    fl::WorldBroadcasterHooks h_broadcaster;
    TestAdminChannel admin(
        h_broadcaster, [](std::string_view, const CommandIssuer&) -> std::string { return "queued"; }, t);
    fl::WorldBroadcaster broadcaster(em, registry, net, log, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");

    ShellDrainMock shell;
    admin.attachShell(shell);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();

    // Command A at t=0 ms; its deadline is t=20 ms.
    auto cmd1 = makeAdminCmd("secret", "spawn", 0x0001u);
    broadcaster.onReceive(0u, cmd1.data(), cmd1.size());
    shell.lines.push_back("[admin] output-A");

    // Advance to t=25 ms; command A's deadline (20 ms) has already passed.
    t.advance(std::chrono::milliseconds(25));

    // Command B at t=25 ms; its deadline is t=45 ms.
    auto cmd2 = makeAdminCmd("secret", "kill", 0x0002u);
    broadcaster.onReceive(0u, cmd2.data(), cmd2.size());
    std::size_t sendsAfterB = net.sends.size();

    // Tick at t=25 ms: only A fires (deadline passed); B is still pending.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto drainAfterTick1 = drainSends(net, sendsAfterB);
    REQUIRE(drainAfterTick1.size() == 1u);
    fl::MsgAdminResponse r{};
    REQUIRE(drainAfterTick1[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&r, drainAfterTick1[0].data(), sizeof(r));
    CHECK(r.reqId == 0x0001u);

    // B's deadline not yet reached; advance only 10 ms more (t=35 ms < 45 ms).
    std::size_t sendsAfterTick1Total = net.sends.size();
    t.advance(std::chrono::milliseconds(10));
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK(drainSends(net, sendsAfterTick1Total).empty());

    // Advance past B's deadline (t=45 ms + 1 ms); B fires.
    std::size_t sendsBeforeB = net.sends.size();
    t.advance(std::chrono::milliseconds(11));
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto drainB = drainSends(net, sendsBeforeB);
    REQUIRE(drainB.size() == 1u);
    fl::MsgAdminResponse r2{};
    REQUIRE(drainB[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&r2, drainB[0].data(), sizeof(r2));
    CHECK(r2.reqId == 0x0002u);
}

TEST_CASE("WorldBroadcaster: spawn position preserves sub-mm precision at large world offset", "[world_broadcaster]") {
    // At x = 1e5 m, float ULP is ~0.0119 m — a 1 mm fractional component would be
    // rounded away when storing into float pos_world.  With double pos_world the
    // 1 mm offset must survive the spawn -> integrator -> broadcast round-trip.
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{1e5 + 1e-3, 500.0, 0.0}});
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // World positions are double throughout the sim, and the shared quantization origin (#725) is
    // carried as a double, so a large coordinate round-trips to within the 0.125 m quantization step
    // rather than being float-truncated (which loses metres at planet scale). The own entity is now
    // quantized like every other entity — the near-exact own position was an artifact of the old
    // per-peer frameOrigin == own position. Lateral gravity (~4e-7 m/tick) is negligible.
    CHECK(e.pos[0] == Catch::Approx(1e5 + 1e-3).margin(fl::kPosStepM));
}

// ---------------------------------------------------------------------------
// Heartbeat / MsgPeerDelay tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: MsgHeartbeat triggers MsgPeerDelay reply", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 70u); // m_currentTick = 70

    const std::size_t sendsBefore = net.sends.size();

    fl::MsgHeartbeat hb;
    hb.tickIndex = 10u; // delay = 70 - 10 = 60 ticks
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    REQUIRE(net.sends.size() == sendsBefore + 1u);
    const auto& reply = net.sends.back();
    REQUIRE(reply.size() == sizeof(fl::MsgPeerDelay));
    fl::MsgPeerDelay pd;
    std::memcpy(&pd, reply.data(), sizeof(pd));
    CHECK(pd.msgId == static_cast<uint8_t>(fl::MsgId::PeerDelay));
    CHECK(pd.delayTicks == 60u);
    CHECK(!net.sendReliable); // must be unreliable
}

TEST_CASE("WorldBroadcaster: MsgHeartbeat with future tickIndex does not update delay", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 50u); // m_currentTick = 50

    fl::MsgHeartbeat hb;
    hb.tickIndex = 60u; // future tick: 60 > 50 — server must ignore
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    // A MsgPeerDelay is still sent but delayTicks should be 0 (estimate not updated)
    REQUIRE(!net.sends.empty());
    fl::MsgPeerDelay pd;
    std::memcpy(&pd, net.sends.back().data(), sizeof(pd));
    CHECK(pd.delayTicks == 0u);
}

TEST_CASE("WorldBroadcaster: MsgHeartbeat caps delayTicks at uint16 max", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    // Drive to a high tick so estimatedDelayTicks will exceed 65535
    broadcaster.onTick(1.0 / 60.0, 70000u);

    fl::MsgHeartbeat hb;
    hb.tickIndex = 0u; // delay = 70000 - 0 = 70000 > 65535
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    REQUIRE(!net.sends.empty());
    fl::MsgPeerDelay pd;
    std::memcpy(&pd, net.sends.back().data(), sizeof(pd));
    CHECK(pd.delayTicks == 65535u);
}

TEST_CASE("WorldBroadcaster: truncated MsgHeartbeat is discarded", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    const std::size_t sendsBefore = net.sends.size();

    uint8_t tiny[4] = {static_cast<uint8_t>(fl::MsgId::Heartbeat), 0, 0, 0};
    broadcaster.onReceive(0u, tiny, sizeof(tiny));

    CHECK(net.sends.size() == sendsBefore); // no reply sent
}

TEST_CASE("WorldBroadcaster: two peers each receive their own MsgPeerDelay", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    broadcaster.onTick(1.0 / 60.0, 100u); // m_currentTick = 100

    // Peer 0: tickIndex=40 → delay = 60; peer 1: tickIndex=70 → delay = 30
    const std::size_t sendsBefore = net.sends.size();

    fl::MsgHeartbeat hb0;
    hb0.tickIndex = 40u;
    broadcaster.onReceive(0u, &hb0, sizeof(hb0));

    fl::MsgHeartbeat hb1;
    hb1.tickIndex = 70u;
    broadcaster.onReceive(1u, &hb1, sizeof(hb1));

    REQUIRE(net.sends.size() == sendsBefore + 2u);

    fl::MsgPeerDelay pd0, pd1;
    std::memcpy(&pd0, net.sends[sendsBefore].data(), sizeof(pd0));
    std::memcpy(&pd1, net.sends[sendsBefore + 1].data(), sizeof(pd1));
    CHECK(pd0.delayTicks == 60u);
    CHECK(pd1.delayTicks == 30u);
}

// ---------------------------------------------------------------------------
// Idle timeout tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: idle timeout 0 never kicks", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(0); // disabled

    connectPilotPeer(broadcaster, net, 0u);
    for (uint64_t t = 1; t <= 600; ++t)
        broadcaster.onTick(1.0 / 60.0, t);

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: idle timeout disconnects peer after inactivity", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(1); // 1 second = 60 ticks

    connectPilotPeer(broadcaster, net, 0u); // lastActivityTick = m_currentTick = 0

    // Run 59 ticks (delay 59 < 60): no kick
    for (uint64_t t = 1; t <= 59; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK(net.disconnectedPeers.empty());

    // Tick 60: delay = 60 >= 60 → kick
    broadcaster.onTick(1.0 / 60.0, 60u);
    CHECK(!net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: MsgHeartbeat resets idle timer", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(1); // 60 ticks

    connectPilotPeer(broadcaster, net, 0u); // lastActivityTick = 0

    // Tick 55: still within window (55 < 60)
    for (uint64_t t = 1; t <= 55; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    REQUIRE(net.disconnectedPeers.empty());

    // Send a heartbeat at tick 55: resets lastActivityTick to 55
    fl::MsgHeartbeat hb;
    hb.tickIndex = 30u; // doesn't matter for the idle reset test
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    // Ticks 56..114: delay from 55 is at most 114-55=59 < 60 → no kick
    for (uint64_t t = 56; t <= 114; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK(net.disconnectedPeers.empty());

    // Tick 115: 115-55=60 >= 60 → kick
    broadcaster.onTick(1.0 / 60.0, 115u);
    CHECK(!net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: MsgClientInput resets idle timer", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(1); // 60 ticks

    connectPilotPeer(broadcaster, net, 0u);

    // Tick 55: no kick yet
    for (uint64_t t = 1; t <= 55; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    REQUIRE(net.disconnectedPeers.empty());

    // Send a MsgClientInput at tick 55: resets lastActivityTick to 55
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.seqNum = 1u;
    inp.tickIndex = 40u;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Ticks 56..114: no kick (max delay = 114-55 = 59 < 60)
    for (uint64_t t = 56; t <= 114; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// SpatialIndex integration
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: spatialIndex is populated with live entity count after onTick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    em.spawn("builtin:debug-entity", t);
    em.spawn("builtin:debug-entity", t);
    em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(broadcaster.spatialIndex().entityCount() == 3u);
}

// Spy controller: captures the AiTickContext received via sample().
struct SpyController : fl::IEntityController {
    fl::AiTickContext lastCtx{};
    bool sampled{false};
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext& ctx) override {
        lastCtx = ctx;
        sampled = true;
        return {};
    }
};

TEST_CASE("WorldBroadcaster: sample receives the AiTickContext from onTick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    auto spy = std::make_unique<SpyController>();
    SpyController* spyPtr = spy.get();
    broadcaster.registerController(id, std::move(spy));

    broadcaster.onTick(1.0 / 60.0, 1u);

    // si must be non-null and the index must already hold the entity (rebuilt before sample())
    REQUIRE(spyPtr->sampled);
    REQUIRE(spyPtr->lastCtx.si != nullptr);
    CHECK(spyPtr->lastCtx.si->entityCount() == 1u);

    // The sensing pass (#685) fills in contacts and env: every controlled entity is an observer, so
    // its table exists — and it is EMPTY here (nothing else is in the world), which is the meaningful
    // distinction from null. Null would mean "sensing was not evaluated for me"; empty means "my
    // sensors ran and found nothing", and a controller must not confuse the two.
    REQUIRE(spyPtr->lastCtx.contacts != nullptr);
    CHECK(spyPtr->lastCtx.contacts->empty());
    CHECK(spyPtr->lastCtx.env != nullptr);

    // Difficulty stays null until a scaling is set (#682): unset = no scaling, NOT AiScaling{},
    // whose defaults are the Cadet preset and would silently halve every radar range.
    CHECK(spyPtr->lastCtx.difficulty == nullptr);
}

TEST_CASE("WorldBroadcaster: an injected AiScaling reaches the controller through the context", "[world_broadcaster]") {
    // #682: difficulty is resolved by fl-server (from a mod-overridable difficulty.toml) and injected
    // here, rather than invented in the engine. A controller sees it through ctx.difficulty; the
    // sensing pass applies radarSensorRange to radar ranges and reactionTimeS to the reaction delay.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    const fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::AiScaling scaling{};
    scaling.radarSensorRange = 0.25f;
    scaling.reactionTimeS = 3.5f;
    broadcaster.setAiScaling(scaling);

    auto spy = std::make_unique<SpyController>();
    SpyController* spyPtr = spy.get();
    broadcaster.registerController(id, std::move(spy));

    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(spyPtr->sampled);
    REQUIRE(spyPtr->lastCtx.difficulty != nullptr);
    CHECK(spyPtr->lastCtx.difficulty->radarSensorRange == Catch::Approx(0.25f));
    CHECK(spyPtr->lastCtx.difficulty->reactionTimeS == Catch::Approx(3.5f));
}

// ---------------------------------------------------------------------------
// Interest management + delta compression tests (#346)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: auto spatial cell size resolves from draw distance (#573)",
          "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::WorldBroadcasterConfig cfg;
    cfg.drawDistanceKm = 200.f; // 200 km → auto = clamp(200000/32, 500, 10000) = 6250 m
    cfg.spatialCellSizeM = 0.0; // auto
    broadcaster.applyConfig(cfg);
    CHECK(broadcaster.spatialIndex().cellSizeM() == Catch::Approx(6250.0));

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(totalEntityCount(hdr) >= 1u); // interest still works with the auto cell size
}

TEST_CASE("WorldBroadcaster: two peers at different positions see disjoint entity sets",
          "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(5.f); // 5 km radius

    // Peer 0 spawns near origin; peer 1 spawns 100 km away
    broadcaster.setSpawnPoints({std::array<double, 3>{0.0, 500.0, 0.0}, std::array<double, 3>{100'000.0, 500.0, 0.0}});
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps0 = snapshotsFor(net, 0);
    auto snaps1 = snapshotsFor(net, 1);
    REQUIRE(!snaps0.empty());
    REQUIRE(!snaps1.empty());

    // Each peer should see exactly their own entity (1 entity each, 100 km apart)
    CHECK(totalEntityCount(parseSnapshotHeader(snaps0[0])) == 1u);
    CHECK(totalEntityCount(parseSnapshotHeader(snaps1[0])) == 1u);
}

TEST_CASE("WorldBroadcaster: 3D interest cull rejects an entity far in altitude (#402)",
          "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // The peer spawns near (0, ~500, 0). Place two entities at the SAME XZ (same spatial-hash cell,
    // so the conservative XZ query returns both) but different altitudes.
    fl::EntityTransform near{};
    near.pos[0] = 0.0;
    near.pos[1] = 700.0; // ~200 m above the peer — inside a 1 km sphere
    near.pos[2] = 0.0;
    const uint32_t nearIdx = em.spawn("builtin:debug-entity", near).index;

    fl::EntityTransform high{};
    high.pos[0] = 0.0;
    high.pos[1] = 6000.0; // ~5.5 km above the peer — outside a 1 km sphere despite same XZ cell
    high.pos[2] = 0.0;
    const uint32_t highIdx = em.spawn("builtin:debug-entity", high).index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(1.f); // 1 km interest sphere
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    bool sawNear = false, sawHigh = false;
    for (const auto& e : decodeEntities(snaps[0])) {
        if (e.entityIdx == nearIdx)
            sawNear = true;
        if (e.entityIdx == highIdx)
            sawHigh = true;
    }
    CHECK(sawNear);       // within the 3D sphere
    CHECK_FALSE(sawHigh); // culled by the XYZ distance gate (would pass an XZ-only check)
}

TEST_CASE("WorldBroadcaster: applyConfig propagates drawDistanceKm", "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform far{};
    far.pos[0] = 20'000.0; // 20 km away — in a different spatial hash cell (default cell = 10 km)
    far.pos[1] = 500.0;
    auto farId = em.spawn("builtin:debug-entity", far);
    const uint32_t farIdx = farId.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::WorldBroadcasterConfig cfg;
    cfg.drawDistanceKm = 1.f; // 1 km — only queries cell at peer origin; 20 km entity is in a different cell
    broadcaster.applyConfig(cfg);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    for (const auto& e : parseFullEntries(snaps[0]))
        CHECK(e.entityIdx != farIdx);
}

TEST_CASE("WorldBroadcaster: first tick sends full entries, second tick sends updates", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 1: all entities new — must be full entries
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps1 = snapshotsFor(net, 0);
    REQUIRE(!snaps1.empty());
    CHECK(fullRecordCount(snaps1[0]) >= 1u);
    CHECK(deltaRecordCount(snaps1[0]) == 0u);

    // Client acks tick 1, then tick 2: identities are confirmed — must be delta records.
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps2 = snapshotsFor(net, 0);
    REQUIRE(!snaps2.empty());
    CHECK(fullRecordCount(snaps2[0]) == 0u);
    CHECK(deltaRecordCount(snaps2[0]) >= 1u);
}

TEST_CASE("WorldBroadcaster: entity stays full every tick until the client acks", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // No ack: the peer's identity is unconfirmed, so the record is re-sent full every tick (loss
    // recovery — whatever snapshot the client first receives carries the full).
    for (uint64_t tick = 1; tick <= 5; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        CHECK(fullRecordCount(snaps[0]) >= 1u);
        CHECK(deltaRecordCount(snaps[0]) == 0u);
    }

    // The client acks tick 5; subsequent ticks converge to deltas.
    ackTick(broadcaster, 0u, 5u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 6u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(fullRecordCount(snaps[0]) == 0u);
    CHECK(deltaRecordCount(snaps[0]) >= 1u);
}

TEST_CASE("WorldBroadcaster: a dropped full keeps re-sending full until a later tick is acked",
          "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Ticks 1-3 sent but the client's acks stall at 0 (e.g. the full packets were lost): every tick
    // re-sends a full so the first packet the client does receive carries the identity.
    for (uint64_t tick = 1; tick <= 3; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        CHECK(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);
    }

    // The client finally receives and acks tick 3: the contiguous full streak started at tick 1,
    // and 3 >= 1, so it converges to deltas.
    ackTick(broadcaster, 0u, 3u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 4u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) == 0u);
    CHECK(deltaRecordCount(snapshotsFor(net, 0).back()) >= 1u);
}

// -- Selective-ack identity precision (#566) ---------------------------------------------------------

TEST_CASE("WorldBroadcaster: acking a later tick does not confirm a full whose streak-start tick was "
          "not decoded",
          "[world_broadcaster][identity-ack]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Full sent every tick 1-3, streak frozen at tick 1 (contiguous run, no acks yet).
    for (uint64_t tick = 1; tick <= 3; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        CHECK(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);
    }

    // The client acks tick 3 but its selective-ack mask reports tick 1 (the streak start) was NOT
    // decoded — age = 3 - 1 = 2 → bit 1 cleared. A pre-#566 high-water mark (1 <= 3) would have falsely
    // confirmed the identity and dropped to an undecodable delta; selective-ack keeps re-sending full.
    ackTick(broadcaster, 0u, 3u, 1u, 0xFFFFFFFFu & ~(1u << 1));
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 4u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);
    CHECK(deltaRecordCount(snapshotsFor(net, 0).back()) == 0u);

    // Now the client acks tick 4 with the streak-start bit set (age = 4 - 1 = 3 → bit 2 of the full
    // mask): the identity is confirmed and the entity converges to a delta.
    ackTick(broadcaster, 0u, 4u, 2u, 0xFFFFFFFFu);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 5u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) == 0u);
    CHECK(deltaRecordCount(snapshotsFor(net, 0).back()) >= 1u);
}

TEST_CASE("WorldBroadcaster: entity gen change forces a full entry", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    auto id1 = em.spawn("builtin:debug-entity", t);
    REQUIRE(id1.valid());
    const uint32_t slotIdx = id1.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 1: entity appears as full entry, gen is cached
    broadcaster.onTick(1.0 / 60.0, 1u);
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);

    // Kill the entity and spawn a new one — new entity may reuse the same pool slot with a new gen
    em.kill(id1);
    em.onTick(1.0 / 60.0, 0u); // reap
    auto id2 = em.spawn("builtin:debug-entity", t);
    REQUIRE(id2.valid());
    // id2 may or may not have the same index as id1; if it does, gen is different
    // Either way, any newly spawned entity appears as a full entry because it's not in knownGens
    broadcaster.onTick(1.0 / 60.0, 2u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    // The new entity (possibly same slot, new gen) must appear as a full entry
    bool newEntityIsFullEntry = false;
    for (const auto& e : parseFullEntries(snaps[0]))
        if (e.entityIdx == id2.index && e.entityGen == id2.generation)
            newEntityIsFullEntry = true;
    CHECK(newEntityIsFullEntry);
    (void)slotIdx;
}

TEST_CASE("WorldBroadcaster: reconnect after disconnect starts with fresh known-gen state",
          "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 1: entity known, cached; client acks it
    broadcaster.onTick(1.0 / 60.0, 1u);
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);

    // Tick 2: entity should be update entry
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        CHECK(deltaRecordCount(snaps[0]) >= 1u);
    }
    clearSnapshots(net);

    // Disconnect clears knownGens and ackedTick; reconnect gives fresh state
    broadcaster.onDisconnect(0u);
    connectPilotPeer(broadcaster, net, 0u); // new peer gets peerId=0 again (TrackingNetwork reuses it)

    // Tick 3: fresh connection — all entities must be full entries again
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(fullRecordCount(snaps[0]) >= 1u);
    CHECK(deltaRecordCount(snaps[0]) == 0u);
}

TEST_CASE("WorldBroadcaster: totalEntityCount matches buffer content", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 1 → full entries
    broadcaster.onTick(1.0 / 60.0, 1u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        const auto& pkt = snaps[0];
        auto hdr = parseSnapshotHeader(pkt);
        const std::size_t expectedSize =
            sizeof(fl::MsgWorldSnapshotHeader) + static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) +
            hdr.bitstreamBytes + 6u + // SnapshotPeerCount TLV (estimatedDelayTicks == 0; SnapshotPeerLatency absent)
            kParkedGearArtTlvBytes;
        CHECK(pkt.size() == expectedSize);
    }
    clearSnapshots(net);

    // Tick 2 → update entries
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        const auto& pkt = snaps[0];
        auto hdr = parseSnapshotHeader(pkt);
        const std::size_t expectedSize = sizeof(fl::MsgWorldSnapshotHeader) +
                                         static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) +
                                         hdr.bitstreamBytes + 6u;
        CHECK(pkt.size() == expectedSize);
    }
}

TEST_CASE("WorldBroadcaster: no connected peers produces no snapshot sends", "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    // No onConnect — m_peerEntities is empty; per-peer loop does nothing
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(net.perPeerSends.empty());
}

// ---------------------------------------------------------------------------
// SnapshotPeerLatency TLV tests (#382)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// JitterBuffer unit tests
// ---------------------------------------------------------------------------

TEST_CASE("JitterBuffer: pop on empty buffer returns false", "[jitter_buffer]") {
    fl::JitterBuffer buf;
    fl::BufferedInput out{};
    out.throttle = 0.5f;
    CHECK_FALSE(buf.pop(out));
    CHECK(out.throttle == 0.5f); // unchanged
    CHECK(buf.empty());
    CHECK(buf.size() == 0u);
}

TEST_CASE("JitterBuffer: push then pop returns same values", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    fl::BufferedInput in{};
    in.throttle = 0.8f;
    in.elevator = -0.3f;
    in.aileron = 0.1f;
    in.rudder = -0.05f;
    in.buttons = 0x03u;
    buf.push(in);
    CHECK(buf.size() == 1u);

    fl::BufferedInput out{};
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == 0.8f);
    CHECK(out.elevator == -0.3f);
    CHECK(out.aileron == 0.1f);
    CHECK(out.rudder == -0.05f);
    CHECK(out.buttons == 0x03u);
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: FIFO ordering", "[jitter_buffer]") {
    fl::JitterBuffer buf{8};
    for (uint32_t i = 0; i < 3; ++i) {
        fl::BufferedInput in{};
        in.throttle = static_cast<float>(i + 1) * 0.1f;
        buf.push(in);
    }
    CHECK(buf.size() == 3u);

    for (uint32_t i = 0; i < 3; ++i) {
        fl::BufferedInput out{};
        REQUIRE(buf.pop(out));
        CHECK(out.throttle == Catch::Approx(static_cast<float>(i + 1) * 0.1f));
    }
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: overflow drops oldest", "[jitter_buffer]") {
    fl::JitterBuffer buf{2};
    fl::BufferedInput a{}, b{}, c{};
    a.throttle = 0.1f;
    b.throttle = 0.2f;
    c.throttle = 0.3f;
    buf.push(a);
    buf.push(b);
    buf.push(c); // overflow: a is dropped
    CHECK(buf.size() == 2u);

    fl::BufferedInput out{};
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.2f)); // b first
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.3f)); // c second
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: size tracks correctly with interleaved push and pop", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    fl::BufferedInput dummy{};
    buf.push(dummy);
    buf.push(dummy);
    CHECK(buf.size() == 2u);
    buf.pop(dummy);
    CHECK(buf.size() == 1u);
    buf.push(dummy);
    buf.push(dummy);
    CHECK(buf.size() == 3u);
    buf.pop(dummy);
    buf.pop(dummy);
    buf.pop(dummy);
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: setMaxDepth truncates when smaller than current size", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    fl::BufferedInput in{};
    for (uint32_t i = 0; i < 4; ++i) {
        in.throttle = static_cast<float>(i + 1) * 0.1f;
        buf.push(in);
    }
    CHECK(buf.size() == 4u);
    buf.setMaxDepth(2);
    CHECK(buf.size() == 2u);
    CHECK(buf.maxDepth() == 2u);

    // Oldest two (0.1, 0.2) were dropped; remaining are 0.3, 0.4 in order.
    fl::BufferedInput out{};
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.3f));
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.4f));
}

TEST_CASE("JitterBuffer: setMaxDepth to 0 is clamped to 1", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    buf.setMaxDepth(0u);
    CHECK(buf.maxDepth() == 1u);
}

TEST_CASE("JitterBuffer: ring index wraps correctly at kHardMaxDepth", "[jitter_buffer]") {
    fl::JitterBuffer buf{fl::JitterBuffer::kHardMaxDepth};
    // Push kHardMaxDepth + 3 items; oldest 3 are dropped by overflow.
    for (uint32_t i = 0; i < fl::JitterBuffer::kHardMaxDepth + 3u; ++i) {
        fl::BufferedInput in{};
        in.throttle = static_cast<float>(i) * 0.01f;
        buf.push(in);
    }
    CHECK(buf.size() == fl::JitterBuffer::kHardMaxDepth);

    // Pop all; throttle values should be sequential starting from index 3.
    for (uint32_t i = 0; i < fl::JitterBuffer::kHardMaxDepth; ++i) {
        fl::BufferedInput out{};
        REQUIRE(buf.pop(out));
        CHECK(out.throttle == Catch::Approx(static_cast<float>(i + 3u) * 0.01f));
    }
    CHECK(buf.empty());
}

// ---------------------------------------------------------------------------
// WorldBroadcaster jitter buffer integration tests
// ---------------------------------------------------------------------------

static fl::MsgClientInput makeJitterInput(uint32_t seqNum, float throttle, uint64_t tickIndex = 0u) {
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = seqNum;
    inp.tickIndex = tickIndex;
    inp.throttle = throttle;
    inp.viewAxis[0] = 1.f;
    return inp;
}

TEST_CASE("WorldBroadcaster: received input is buffered and not applied until tick",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    auto inp = makeJitterInput(1u, 0.9f);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Buffer should hold 1 item before any tick drains it.
    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 1u);
}

TEST_CASE("WorldBroadcaster: jitter buffer drains one per tick", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u);
    connectPilotPeer(broadcaster, net, 0u);

    // Advance to tick 10 so the first input gets estimatedDelayTicks=10 → buffer depth=min(10,8)=8.
    broadcaster.onTick(1.0 / 60.0, 10u);

    // Push 3 inputs with distinct seqNums (tickIndex=0 so delay=10).
    for (uint32_t i = 0; i < 3u; ++i) {
        auto inp = makeJitterInput(i + 1u, static_cast<float>(i + 1u) * 0.2f);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    for (uint32_t expected = 3u; expected > 0u; --expected) {
        uint32_t gotDepth = 0xFFu;
        broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
        CHECK(gotDepth == expected);
        broadcaster.onTick(1.0 / 60.0, expected);
    }
    // After 3 ticks the buffer is empty.
    uint32_t finalDepth = 0xFFu;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalDepth = pi.queueDepth; });
    CHECK(finalDepth == 0u);
}

TEST_CASE("WorldBroadcaster: empty buffer tick uses stale repeat without crash", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    auto inp = makeJitterInput(1u, 0.5f);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Tick 1: drains the one buffered input.
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Tick 2: buffer is empty — stale repeat; entity must remain live (no crash).
    broadcaster.onTick(1.0 / 60.0, 2u);

    fl::EntityId eid;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { eid = pi.eid; });
    CHECK(eid.valid());
}

TEST_CASE("WorldBroadcaster: forEachPeer reports queueDepth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u);
    connectPilotPeer(broadcaster, net, 0u);

    // Advance to tick 10 so the first input seeds buffer depth=min(10,8)=8.
    broadcaster.onTick(1.0 / 60.0, 10u);

    auto i1 = makeJitterInput(1u, 0.3f);
    auto i2 = makeJitterInput(2u, 0.6f);
    broadcaster.onReceive(0u, &i1, sizeof(i1));
    broadcaster.onReceive(0u, &i2, sizeof(i2));

    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 2u);
}

TEST_CASE("WorldBroadcaster: jitter buffer depth seeded from estimatedDelayTicks",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u); // global max = 8
    connectPilotPeer(broadcaster, net, 0u);

    // Advance to tick 10 so delay = 10 - 5 = 5 ticks.
    broadcaster.onTick(1.0 / 60.0, 10u);
    clearSnapshots(net);

    // First input seeds buffer depth = min(5, 8) = 5.
    auto inp = makeJitterInput(1u, 0.5f, 5u);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Push 5 more inputs to fill the buffer; the 6th should overflow (depth=5).
    for (uint32_t i = 2u; i <= 6u; ++i) {
        auto extra = makeJitterInput(i, 0.1f * static_cast<float>(i));
        broadcaster.onReceive(0u, &extra, sizeof(extra));
    }
    // Buffer should hold exactly 5 (depth cap).
    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 5u);
}

TEST_CASE("WorldBroadcaster: jitter buffer depth capped at jitterMaxDepth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(2u); // hard cap = 2
    connectPilotPeer(broadcaster, net, 0u);

    // Advance to tick 20 so delay = 20 - 0 = 20, which would give depth 20 uncapped.
    broadcaster.onTick(1.0 / 60.0, 20u);
    clearSnapshots(net);

    // First input: delay=20 but cap=2, so depth = min(20,2) = 2.
    auto inp = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Push 2 more; buffer should saturate at 2 and overflow.
    auto i2 = makeJitterInput(2u, 0.6f);
    auto i3 = makeJitterInput(3u, 0.7f);
    broadcaster.onReceive(0u, &i2, sizeof(i2));
    broadcaster.onReceive(0u, &i3, sizeof(i3)); // overflow

    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 2u);
}

TEST_CASE("WorldBroadcaster: jitter buffers are independent per peer", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u);
    net.peerAddresses[0] = "1.1.1.1:1000";
    net.peerAddresses[1] = "2.2.2.2:2000";
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);

    // Advance to tick 10 so first input from each peer gets depth=min(10,8)=8.
    broadcaster.onTick(1.0 / 60.0, 10u);

    // Send 2 inputs to peer 0, 1 input to peer 1 (tickIndex=0 → delay=10).
    auto a1 = makeJitterInput(1u, 0.1f);
    auto a2 = makeJitterInput(2u, 0.2f);
    auto b1 = makeJitterInput(1u, 0.5f);
    broadcaster.onReceive(0u, &a1, sizeof(a1));
    broadcaster.onReceive(0u, &a2, sizeof(a2));
    broadcaster.onReceive(1u, &b1, sizeof(b1));

    std::map<uint32_t, uint32_t> depths;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { depths[pi.peerId] = pi.queueDepth; });
    CHECK(depths[0u] == 2u);
    CHECK(depths[1u] == 1u);
}

TEST_CASE("WorldBroadcaster: setJitterBufferDepth affects initial depth for new peers",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(6u);
    net.peerAddresses[0] = "1.1.1.1:1000";
    net.peerAddresses[1] = "2.2.2.2:2000";

    // Advance to tick 10 so delay estimates are non-zero.
    broadcaster.onTick(1.0 / 60.0, 10u);
    clearSnapshots(net);

    // Peer 0 connects, sends input with tickIndex=0 (delay=10, cap=6 -> depth=6).
    connectPilotPeer(broadcaster, net, 0u);
    auto inp0 = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Change the global max.
    broadcaster.setJitterBufferDepth(3u);

    // Peer 1 connects after the change, sends input with tickIndex=0 (delay=10, cap=3 -> depth=3).
    connectPilotPeer(broadcaster, net, 1u);
    auto inp1 = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(1u, &inp1, sizeof(inp1));

    // Fill both buffers to their respective caps.
    for (uint32_t i = 2u; i <= 7u; ++i) {
        auto extra = makeJitterInput(i, 0.1f);
        broadcaster.onReceive(0u, &extra, sizeof(extra));
    }
    for (uint32_t i = 2u; i <= 4u; ++i) {
        auto extra = makeJitterInput(i, 0.1f);
        broadcaster.onReceive(1u, &extra, sizeof(extra));
    }

    std::map<uint32_t, uint32_t> depths;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { depths[pi.peerId] = pi.queueDepth; });
    // Peer 0 was seeded with depth=6 before the change.
    CHECK(depths[0u] == 6u);
    // Peer 1 was seeded with depth=3 after the change.
    CHECK(depths[1u] == 3u);
}

// ---------------------------------------------------------------------------
// Adaptive jitter buffer tests (#424 + #429)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: EWMA delay seeded from first estimatedDelayTicks", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5, fast convergence
    broadcaster.setJitterHysteresis(0u);  // resize on any diff
    broadcaster.setJitterMultiplier(0.f); // delay-only
    connectPilotPeer(broadcaster, net, 0u);

    // Advance to tick 6 so first input delay = 6 - 1 = 5.
    broadcaster.onTick(1.0 / 60.0, 6u);
    clearSnapshots(net);

    auto inp = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // After seeding: EWMA = 5, depth = 5. Verify via forEachPeer.
    float gotEwma = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotEwma = pi.ewmaDelayTicks; });
    CHECK(gotEwma == Catch::Approx(5.f));
}

TEST_CASE("WorldBroadcaster: EWMA delay converges toward new samples", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=2 (tick=3, tickIndex=1).
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Send many inputs at delay=20 (tickIndex=0, server still at tick 3 after onTick above).
    // EWMA update: tick stays at 3 but subsequent inputs use seqNums to track.
    // Send more inputs advancing server to tick 23 so delay = 23 - 1 = 22 each time.
    for (uint32_t seq = 2u; seq <= 12u; ++seq) {
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(3u + seq));
        clearSnapshots(net);
        auto inp = makeJitterInput(seq, 0.5f, 1u); // tickIndex=1, delay grows with server tick
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    // After 10 updates at large delay, EWMA has moved well above 2.
    float gotEwma = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotEwma = pi.ewmaDelayTicks; });
    CHECK(gotEwma > 5.f); // well above initial seed
}

TEST_CASE("WorldBroadcaster: adaptive resize grows buffer when EWMA delay increases",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5, 8 samples for ~99% convergence
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=2 (depth=2).
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));
    broadcaster.onTick(1.0 / 60.0, 4u); // resize check: target=2, current=2, no resize
    clearSnapshots(net);

    // Now send inputs at high delay (tickIndex=0 so delay = serverTick - 0 = serverTick).
    // With alpha=0.5 and 10 updates at delay=12, EWMA approaches 12 asymptotically.
    for (uint32_t seq = 2u; seq <= 12u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(4u + seq));
        clearSnapshots(net);
    }

    // Buffer should have grown from 2 to at least 6 (EWMA ~= 4 after 10 iterations seeded at 2).
    // Push 30 inputs and check queueDepth cap reflects growth.
    for (uint32_t seq = 13u; seq <= 42u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }
    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax > 2u);
}

TEST_CASE("WorldBroadcaster: adaptive resize shrinks buffer when delay drops", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=12 (depth=12).
    broadcaster.onTick(1.0 / 60.0, 13u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));
    broadcaster.onTick(1.0 / 60.0, 14u);
    clearSnapshots(net);

    // Now drive delay to 1 with many updates (tickIndex = serverTick-1 each time).
    for (uint32_t seq = 2u; seq <= 20u; ++seq) {
        uint64_t tick = static_cast<uint64_t>(14u + seq);
        auto inp = makeJitterInput(seq, 0.5f, tick - 1u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax < 12u); // buffer shrank
    CHECK(finalMax >= 1u); // never below floor
}

TEST_CASE("WorldBroadcaster: hysteresis prevents resize for small EWMA drift", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(8u); // large dead-band
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=5 (depth=5).
    broadcaster.onTick(1.0 / 60.0, 6u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));
    broadcaster.onTick(1.0 / 60.0, 7u);
    clearSnapshots(net);

    // Send many inputs at delay=7 — EWMA drifts toward 7, but |7-5|=2 < hysteresis=8 → no resize.
    for (uint32_t seq = 2u; seq <= 16u; ++seq) {
        uint64_t tick = static_cast<uint64_t>(7u + seq);
        auto inp = makeJitterInput(seq, 0.5f, tick - 7u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax == 5u); // unchanged due to hysteresis
}

TEST_CASE("WorldBroadcaster: adaptive resize clamped at jitterMaxDepth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(6u); // global cap = 6
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=30 — capped to 6 at seeding.
    broadcaster.onTick(1.0 / 60.0, 31u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Drive EWMA toward 30 (delay stays large).
    for (uint32_t seq = 2u; seq <= 12u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 1u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(31u + seq));
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax == 6u); // capped at global max
}

TEST_CASE("WorldBroadcaster: setJitterMultiplier 0 gives delay-only depth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f); // pure delay-only
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=4 (depth=4).
    broadcaster.onTick(1.0 / 60.0, 5u);
    clearSnapshots(net);
    // Send inputs at irregular spacings to build up jitter EWMA.
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    broadcaster.onTick(1.0 / 60.0, 6u);
    clearSnapshots(net);
    // Skip a few ticks to create jitter, then send at delay=4.
    auto inp1 = makeJitterInput(2u, 0.5f, 2u);
    broadcaster.onReceive(0u, &inp1, sizeof(inp1));
    broadcaster.onTick(1.0 / 60.0, 7u);
    clearSnapshots(net);

    // Even with jitter EWMA > 0, multiplier=0 means it has no effect on target.
    // EWMA delay ≈ 4 (delay stayed at ~4), so target should remain 4.
    float gotJitter = -1.f;
    uint32_t gotMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        gotJitter = pi.ewmaJitterTicks;
        gotMax = pi.bufferMaxDepth;
    });
    CHECK(gotMax == 4u);
    (void)gotJitter; // jitter EWMA may be non-zero but has no effect on depth
}

TEST_CASE("WorldBroadcaster: forEachPeer PeerInfo carries bufferMaxDepth after adaptive resize",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=2.
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Drive EWMA to ~10 and let onTick resize.
    for (uint32_t seq = 2u; seq <= 16u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(3u + seq));
        clearSnapshots(net);
    }

    uint32_t gotMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotMax = pi.bufferMaxDepth; });
    CHECK(gotMax > 2u); // PeerInfo reflects updated bufferMaxDepth
}

TEST_CASE("WorldBroadcaster: adaptive resize skips peer with no EWMA sample", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // onTick without any input from this peer — must not crash.
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);

    // forEachPeer should still work; EWMA fields default to zero.
    float gotEwma = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotEwma = pi.ewmaDelayTicks; });
    CHECK(gotEwma == 0.f);
}

TEST_CASE("WorldBroadcaster: adaptive resize floors target at 1 with zero delay",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // First input at delay=0 (tickIndex == m_currentTick after onTick(1)).
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);
    auto inp = makeJitterInput(1u, 0.5f, 1u); // tickIndex=1, m_currentTick=1, delay=0
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Resize check: EWMA=0, target = clamp(ceil(0),1,32) = 1.
    broadcaster.onTick(1.0 / 60.0, 2u);
    clearSnapshots(net);

    uint32_t gotMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotMax = pi.bufferMaxDepth; });
    CHECK(gotMax == 1u);
}

TEST_CASE("WorldBroadcaster: jitter EWMA stays near zero for regular 1-tick-spaced inputs",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterAdaptWindow(4u);
    broadcaster.setJitterMultiplier(1.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Send 20 inputs, each exactly 1 server tick apart.
    for (uint32_t seq = 1u; seq <= 20u; ++seq) {
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(seq));
        clearSnapshots(net);
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    float gotJitter = 1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotJitter = pi.ewmaJitterTicks; });
    // Inputs arrive exactly 1 tick apart → deviation = |1 - 1| = 0 each time → EWMA stays 0.
    CHECK(gotJitter == Catch::Approx(0.f).margin(0.01f));
}

TEST_CASE("WorldBroadcaster: jitter EWMA grows for irregular arrivals", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterAdaptWindow(4u); // alpha=0.25
    broadcaster.setJitterMultiplier(1.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at tick 1.
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Now send at irregular spacings: ticks 2, 4, 5, 9, 10 (gaps of 2, 1, 4, 1).
    uint64_t irregularTicks[] = {2u, 4u, 5u, 9u, 10u};
    uint32_t seq = 2u;
    for (uint64_t tick : irregularTicks) {
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
        auto inp = makeJitterInput(seq++, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    float gotJitter = 0.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotJitter = pi.ewmaJitterTicks; });
    CHECK(gotJitter > 0.f); // irregular arrivals → non-zero jitter EWMA
}

TEST_CASE("WorldBroadcaster: adaptive resize shrinks buffer and drops excess fill",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=10 (depth=10).
    broadcaster.onTick(1.0 / 60.0, 11u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Fill to 8 entries.
    for (uint32_t seq = 2u; seq <= 8u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    // Verify fill=8, max=10.
    uint32_t preFill = 0u, preMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        preFill = pi.queueDepth;
        preMax = pi.bufferMaxDepth;
    });
    CHECK(preFill == 8u);
    CHECK(preMax == 10u);

    // Now drive EWMA to 3 by sending at delay=3 repeatedly.
    for (uint32_t seq = 9u; seq <= 22u; ++seq) {
        uint64_t tick = static_cast<uint64_t>(11u + seq);
        auto inp = makeJitterInput(seq, 0.5f, tick - 3u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
    }

    // After resize to 3, buffer fill must have been truncated to at most 3.
    uint32_t postFill = 0u, postMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        postFill = pi.queueDepth;
        postMax = pi.bufferMaxDepth;
    });
    CHECK(postMax < 10u);       // shrank
    CHECK(postFill <= postMax); // fill never exceeds new max
}

TEST_CASE("WorldBroadcaster: applyConfig wires jitterAdaptWindow hysteresis multiplier",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.jitterBufferMaxDepth = 32u;
    cfg.jitterAdaptWindow = 2u; // fast convergence
    cfg.jitterHysteresis = 0u;  // resize immediately
    cfg.jitterMultiplier = 0.f; // delay-only
    broadcaster.applyConfig(cfg);

    connectPilotPeer(broadcaster, net, 0u);

    // Seed at delay=2, then drive to delay=10; adaptive resize should fire.
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    for (uint32_t seq = 2u; seq <= 14u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(3u + seq));
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax > 2u); // config was applied: adapt window + hysteresis drove resize
}

// ---------------------------------------------------------------------------
// Priority/budget snapshot scheduler (#516)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: re-entry after retention gap forces a full record",
          "[world_broadcaster][interest][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[0] = 100.0;
    t.pos[1] = 500.0;
    fl::EntityId e = em.spawn("builtin:debug-entity", t);
    const uint32_t eIdx = e.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 1: full record (first sight). After the client acks it, ticks where it stays known → deltas.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto isFullFor = [&](const std::vector<uint8_t>& pkt) {
        for (const auto& d : decodeEntities(pkt))
            if (d.entityIdx == eIdx)
                return d.isFull;
        return false;
    };
    REQUIRE(isFullFor(snapshotsFor(net, 0).back())); // first sight = full

    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK_FALSE(isFullFor(snapshotsFor(net, 0).back())); // known + acked → delta

    // Jump the tick index far past kSnapshotRetentionTicks since lastSentTick (=2): the entity is
    // re-sent as a full record because the client may have evicted it.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u + fl::kSnapshotRetentionTicks + 5u);
    CHECK(isFullFor(snapshotsFor(net, 0).back()));
}

// ---------------------------------------------------------------------------
// Adaptive send-rate / congestion response (#518)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: zero link stats leave every peer at the full per-tick rate",
          "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion()); // enabled, but no link stats injected => zeros
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t tick = 1; tick <= 30; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    CHECK(snapshotsFor(net, 0).size() == 30u); // unchanged from pre-#518 behaviour
}

TEST_CASE("WorldBroadcaster: forEachPeer reports throttled send rate and packet loss",
          "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion());
    connectPilotPeer(broadcaster, net, 0u);
    net.peerLinkStats[0] = lossLink(0.5f);
    for (uint64_t tick = 1; tick <= 40; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    float rate = 60.f;
    float loss = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        rate = pi.sendRateHz;
        loss = pi.packetLoss;
    });
    CHECK(rate < 60.f);                 // decimated
    CHECK(loss == Catch::Approx(0.5f)); // live ENet loss surfaced
}

// Regression (#94 fuzzing): NaN/Inf control floats from an untrusted client must be sanitized before
// entering the sim. std::clamp passes NaN through unchanged, and a NaN throttle later trips UB at the
// float->uint8 telemetry cast during snapshot assembly.
TEST_CASE("WorldBroadcaster: NaN/Inf client input is sanitized, not propagated", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.seqNum = 1;
    inp.throttle = std::nanf("");
    inp.elevator = HUGE_VALF; // +inf
    inp.aileron = -HUGE_VALF; // -inf
    inp.rudder = std::nanf("");
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Stepping assembles a snapshot (the float->uint8 telemetry cast). Sanitized input keeps it UB-free.
    REQUIRE_NOTHROW(broadcaster.onTick(1.0 / 60.0, 1u));
}

// Regression (#993, deep-fuzz fuzz_server_msg): an observer's MsgClientInput::cameraEye is finite-
// guarded in onReceive but not bounded, so an extreme-but-finite value flows into interestCenter and
// then into SpatialIndex::queryRadius, where floor(v)->int64_t past 2^63 is undefined behavior (UBSan
// flagged 9.52682e+135). The spatial index now saturates the cell coordinate; the whole tick must
// stay UB-free and still deliver a snapshot to the observer.
TEST_CASE("WorldBroadcaster: extreme observer camera eye does not trip spatial-hash UB", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger); // observers allowed by default
    connectObserverPeer(broadcaster, net, 0u);

    // A real entity in the world so queryRadius has a populated grid to sweep past.
    fl::EntityTransform t{};
    em.spawn("builtin:debug-entity", t);

    // cameraEye far beyond the int64 cell range on every axis — finite, so onReceive keeps it.
    const double huge = 9.52682e+135;
    fl::MsgClientInput inp = cameraInput(1u, huge, -huge, huge);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // The gather centers queryRadius on interestCenter (= cameraEye) for the entity-less observer.
    REQUIRE_NOTHROW(broadcaster.onTick(1.0 / 60.0, 1u));
}

// ---------------------------------------------------------------------------
// Server-side input tracing (#560)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: input tracing records accepted inputs only", "[world_broadcaster][trace]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "fl_wb_trace_test";
    std::error_code ec;
    fs::remove_all(dir, ec);

    broadcaster.setInputTraceDir(dir.string());
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 5u); // establish m_currentTick = 5 for the recorded serverTick

    // Two accepted inputs bracket a stale (non-newer seq) and a version-mismatch input, both rejected
    // before the trace-record write — so only the two accepted samples land in the trace.
    fl::MsgClientInput a{};
    a.seqNum = 1;
    a.tickIndex = 5;
    a.throttle = 0.5f;
    a.elevator = 0.25f;
    a.buttons = 0x01;
    broadcaster.onReceive(0u, &a, sizeof(a));

    fl::MsgClientInput stale{}; // seqNum not newer -> discarded by the staleness guard
    stale.seqNum = 1;
    stale.tickIndex = 5;
    stale.throttle = 0.9f;
    broadcaster.onReceive(0u, &stale, sizeof(stale));

    fl::MsgClientInput bad{}; // protocol mismatch -> discarded
    bad.seqNum = 2;
    bad.protocolVersion = 999;
    bad.throttle = 0.9f;
    broadcaster.onReceive(0u, &bad, sizeof(bad));

    fl::MsgClientInput b{};
    b.seqNum = 3;
    b.tickIndex = 5;
    b.throttle = 1.0f;
    b.buttons = 0x02;
    broadcaster.onReceive(0u, &b, sizeof(b));

    broadcaster.onDisconnect(0u); // flushes + closes the per-peer trace file

    // Exactly one trace file was produced; parse it and confirm only the accepted inputs are present.
    // (First-entry iterator idiom rather than a range-for with an unconditional break, which MSVC
    // flags as C4702 unreachable-code — the loop continuation can never be reached.)
    fs::directory_iterator dirIt(dir);
    REQUIRE(dirIt != fs::directory_iterator{});
    const fs::path tracePath = dirIt->path();
    REQUIRE_FALSE(tracePath.empty());
    std::ifstream f(tracePath, std::ios::binary);
    REQUIRE(f.good());
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    fl::InputTrace tr;
    std::string err;
    REQUIRE(fl::parseInputTrace(buf.data(), buf.size(), tr, err));
    REQUIRE(tr.records.size() == 2u);
    CHECK(tr.records[0].serverTick == 5u);
    CHECK(tr.records[0].throttle == Catch::Approx(0.5f));
    CHECK(tr.records[0].elevator == Catch::Approx(0.25f));
    CHECK(tr.records[0].buttons == 0x01u);
    CHECK(tr.records[1].throttle == Catch::Approx(1.0f));
    CHECK(tr.records[1].buttons == 0x02u);

    fs::remove_all(dir, ec);
}

TEST_CASE("WorldBroadcaster: tracing disabled writes nothing", "[world_broadcaster][trace]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "fl_wb_trace_off_test";
    std::error_code ec;
    fs::remove_all(dir, ec);

    // Never call setInputTraceDir: tracing is off by default.
    connectPilotPeer(broadcaster, net, 0u);
    ackTick(broadcaster, 0u, 0u, 1u);
    broadcaster.onDisconnect(0u);

    CHECK_FALSE(fs::exists(dir)); // no directory created, no files written
}

// ---------------------------------------------------------------------------
// Congestion telemetry watermarks (#714)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Wingman / flight order channel (#610)
//
// NOTE: this file must stay free of engine-ai (it is in the TSan target set). It can, because the
// order path reaches engine-ai only through std::function hooks — which is exactly why they are
// hooks. The tests fill them with stubs.
// ---------------------------------------------------------------------------

namespace {

// Collect every MsgWingmanAck the server unicast to `peerId`.
std::vector<fl::MsgWingmanAck> acksFor(const MockNetwork& net, uint32_t peerId) {
    std::vector<fl::MsgWingmanAck> out;
    for (const auto& [pid, pkt] : net.perPeerSends) {
        if (pid != peerId || pkt.empty())
            continue;
        if (pkt[0] != static_cast<uint8_t>(fl::MsgId::WingmanAck))
            continue;
        fl::MsgWingmanAck ack;
        if (fl::readMsg(pkt.data(), pkt.size(), ack))
            out.push_back(ack);
    }
    return out;
}

fl::MsgWingmanCommand makeOrder(uint8_t command, uint16_t flightId, uint32_t memberIdx = fl::kFlightAll,
                                uint32_t seq = 1) {
    fl::MsgWingmanCommand m{};
    m.command = command;
    m.flightId = flightId;
    m.memberIdx = memberIdx;
    m.seqNum = seq;
    return m;
}

constexpr uint8_t kRejoin = 2; // fl::ai::WingmanCommand::Rejoin — spelled out to keep engine-ai out

} // namespace

TEST_CASE("WorldBroadcaster: a player entity is stamped with the configured faction", "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setPlayerFaction(7);
    connectPilotPeer(broadcaster, net, 0u);

    bool found = false;
    em.forEach([&](const fl::EntityState& s) {
        if (s.playerOwned || s.ownerId == 0u) {
            // The peer's entity: without a non-zero faction nothing in the world is hostile to it.
            found = true;
            CHECK(s.factionIndex == 7);
        }
    });
    CHECK(found);
}

TEST_CASE("WorldBroadcaster: player faction 0 restores the legacy neutral behavior", "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setPlayerFaction(0);
    connectPilotPeer(broadcaster, net, 0u);

    em.forEach([&](const fl::EntityState& s) { CHECK(s.factionIndex == 0); });
}

TEST_CASE("WorldBroadcaster: the flight check-in tells the client its flight id and size",
          "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster* bcPtr = nullptr; // the spawner calls back in; set below
    fl::WorldQueries q_broadcaster;
    q_broadcaster.flightSpawner = [&](uint32_t peerId, fl::EntityId lead) {
        const fl::FormationId fid = bcPtr->formations().create("Viper", lead, peerId);
        fl::EntityTransform t{};
        const fl::EntityId ai = em.spawn("builtin:debug-entity", t);
        fl::FormationMember m{};
        m.id = ai;
        m.peerId = fl::kNoPeer; // AI
        bcPtr->formations().addMember(fid, m);
        return fid;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));
    bcPtr = &broadcaster;
    connectPilotPeer(broadcaster, net, 0u);

    const auto acks = acksFor(net, 0);
    REQUIRE(acks.size() == 1u);
    CHECK(acks[0].result == static_cast<uint8_t>(fl::WingmanResult::CheckIn));
    CHECK(acks[0].flightSize == 1u);
    CHECK(acks[0].flightId != fl::kNoFlightId);
}

TEST_CASE("WorldBroadcaster: an order retasks an AI member and is acknowledged", "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster* bcPtr = nullptr; // the spawner calls back in; set below
    fl::FormationId theFlight = fl::kNoFormation;
    fl::WorldQueries q_broadcaster;
    int handlerCalls = 0;
    fl::WorldBroadcasterHooks h_broadcaster;
    q_broadcaster.flightSpawner = [&](uint32_t peerId, fl::EntityId lead) {
        theFlight = bcPtr->formations().create("Viper", lead, peerId);
        fl::EntityTransform t{};
        fl::FormationMember m{};
        m.id = em.spawn("builtin:debug-entity", t);
        bcPtr->formations().addMember(theFlight, m);
        return theFlight;
    };
    h_broadcaster.comms.flightOrders = [&](const fl::Formation&, const fl::FormationMember&, uint8_t, fl::EntityId) {
        ++handlerCalls;
        return true;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster),
                                     std::move(h_broadcaster));
    bcPtr = &broadcaster;

    connectPilotPeer(broadcaster, net, 0u);
    net.perPeerSends.clear();

    const auto order = makeOrder(kRejoin, theFlight);
    broadcaster.onReceive(0u, &order, sizeof(order));

    CHECK(handlerCalls == 1);
    const auto acks = acksFor(net, 0);
    REQUIRE(acks.size() == 1u);
    CHECK(acks[0].result == static_cast<uint8_t>(fl::WingmanResult::Acknowledged));
    CHECK(acks[0].command == kRejoin);
}

TEST_CASE("WorldBroadcaster: a peer cannot order a flight it does not command", "[world_broadcaster][wingman]") {
    // THE AUTHORIZATION TEST. Authority comes from commanding the formation, never from the packet —
    // and the refusal is the SAME code as "no such flight", so the order channel cannot be used to
    // enumerate which formations exist or who leads them.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster* bcPtr = nullptr; // the spawner calls back in; set below
    fl::WorldQueries q_broadcaster;
    fl::FormationId peer0Flight = fl::kNoFormation;
    int handlerCalls = 0;
    fl::WorldBroadcasterHooks h_broadcaster;
    q_broadcaster.flightSpawner = [&](uint32_t peerId, fl::EntityId lead) {
        const fl::FormationId fid = bcPtr->formations().create("Viper", lead, peerId);
        fl::EntityTransform t{};
        fl::FormationMember m{};
        m.id = em.spawn("builtin:debug-entity", t);
        bcPtr->formations().addMember(fid, m);
        if (peerId == 0)
            peer0Flight = fid;
        return fid;
    };
    h_broadcaster.comms.flightOrders = [&](const fl::Formation&, const fl::FormationMember&, uint8_t, fl::EntityId) {
        ++handlerCalls;
        return true;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster),
                                     std::move(h_broadcaster));
    bcPtr = &broadcaster;

    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    net.perPeerSends.clear();

    // Peer 1 names peer 0's flight explicitly.
    const auto order = makeOrder(kRejoin, peer0Flight);
    broadcaster.onReceive(1u, &order, sizeof(order));

    CHECK(handlerCalls == 0); // the handler is never reached
    const auto acks = acksFor(net, 1);
    REQUIRE(acks.size() == 1u);
    CHECK(acks[0].result == static_cast<uint8_t>(fl::WingmanResult::NoFlight));
}

TEST_CASE("WorldBroadcaster: an unknown command ordinal is rejected without calling the handler",
          "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster* bcPtr = nullptr; // the spawner calls back in; set below
    fl::FormationId theFlight = fl::kNoFormation;
    fl::WorldQueries q_broadcaster;
    int handlerCalls = 0;
    fl::WorldBroadcasterHooks h_broadcaster;
    q_broadcaster.flightSpawner = [&](uint32_t peerId, fl::EntityId lead) {
        theFlight = bcPtr->formations().create("Viper", lead, peerId);
        fl::EntityTransform t{};
        fl::FormationMember m{};
        m.id = em.spawn("builtin:debug-entity", t);
        bcPtr->formations().addMember(theFlight, m);
        return theFlight;
    };
    h_broadcaster.comms.flightOrders = [&](const fl::Formation&, const fl::FormationMember&, uint8_t, fl::EntityId) {
        ++handlerCalls;
        return true;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster),
                                     std::move(h_broadcaster));
    bcPtr = &broadcaster;

    connectPilotPeer(broadcaster, net, 0u);
    net.perPeerSends.clear();

    const auto order = makeOrder(/*command=*/200, theFlight); // outside the grammar
    broadcaster.onReceive(0u, &order, sizeof(order));

    CHECK(handlerCalls == 0);
    const auto acks = acksFor(net, 0);
    REQUIRE(acks.size() == 1u);
    CHECK(acks[0].result == static_cast<uint8_t>(fl::WingmanResult::Rejected));
}

TEST_CASE("WorldBroadcaster: attack_my_target refuses when nothing is in the boresight cone",
          "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster* bcPtr = nullptr; // the spawner calls back in; set below
    fl::WorldQueries q_broadcaster;
    int handlerCalls = 0;
    fl::FormationId theFlight = fl::kNoFormation;
    fl::WorldBroadcasterHooks h_broadcaster;
    q_broadcaster.flightSpawner = [&](uint32_t peerId, fl::EntityId lead) {
        theFlight = bcPtr->formations().create("Viper", lead, peerId);
        fl::EntityTransform t{};
        fl::FormationMember m{};
        m.id = em.spawn("builtin:debug-entity", t);
        bcPtr->formations().addMember(theFlight, m);
        return theFlight;
    };
    h_broadcaster.comms.flightOrders = [&](const fl::Formation&, const fl::FormationMember&, uint8_t, fl::EntityId) {
        ++handlerCalls;
        return true;
    };
    q_broadcaster.targetDesignator = [](const fl::EntityState&, const float[3]) { return fl::EntityId{}; };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster),
                                     std::move(h_broadcaster));
    bcPtr = &broadcaster;
    // A designator that finds nothing — the same thing an empty sky produces.

    connectPilotPeer(broadcaster, net, 0u);
    net.perPeerSends.clear();

    const auto order = makeOrder(/*attack_my_target=*/0, theFlight);
    broadcaster.onReceive(0u, &order, sizeof(order));

    // Behavior is UNCHANGED and the player is told. An attack order that quietly picks its own target
    // would be worse than one that declines.
    CHECK(handlerCalls == 0);
    const auto acks = acksFor(net, 0);
    REQUIRE(acks.size() == 1u);
    CHECK(acks[0].result == static_cast<uint8_t>(fl::WingmanResult::NoTarget));
    CHECK(acks[0].targetIdx == fl::kNoTarget);
}

TEST_CASE("WorldBroadcaster: an order to a HUMAN member is relayed, not applied", "[world_broadcaster][wingman]") {
    // The server cannot retask a person. It relays the call and tells the commander it was passed on,
    // rather than letting them believe an aircraft is now obeying.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    int handlerCalls = 0;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.comms.flightOrders = [&](const fl::Formation&, const fl::FormationMember&, uint8_t, fl::EntityId) {
        ++handlerCalls;
        return true;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));
    fl::FormationId theFlight = fl::kNoFormation;

    connectPilotPeer(broadcaster, net, 0u); // the commander
    connectPilotPeer(broadcaster, net, 1u); // the human wingman

    // Peer 0 leads a flight whose only member is peer 1's aircraft.
    fl::EntityId lead0, wing1;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        if (pi.peerId == 0)
            lead0 = pi.eid;
        if (pi.peerId == 1)
            wing1 = pi.eid;
    });
    REQUIRE(lead0.valid());
    REQUIRE(wing1.valid());

    theFlight = broadcaster.formations().create("Viper", lead0, /*commander=*/0);
    fl::FormationMember human{};
    human.id = wing1;
    human.peerId = 1; // a PERSON
    broadcaster.formations().addMember(theFlight, human);

    net.perPeerSends.clear();
    const auto order = makeOrder(kRejoin, theFlight);
    broadcaster.onReceive(0u, &order, sizeof(order));

    CHECK(handlerCalls == 0); // no controller was swapped: there is no controller to swap

    // The human member got the radio call...
    const auto memberAcks = acksFor(net, 1);
    REQUIRE(memberAcks.size() == 1u);
    CHECK(memberAcks[0].result == static_cast<uint8_t>(fl::WingmanResult::Relayed));
    CHECK(memberAcks[0].command == kRejoin);
    CHECK(memberAcks[0].memberIdx == lead0.index); // ...and knows who is calling

    // ...and the commander is told it was relayed, not acknowledged.
    const auto leadAcks = acksFor(net, 0);
    REQUIRE(leadAcks.size() == 1u);
    CHECK(leadAcks[0].result == static_cast<uint8_t>(fl::WingmanResult::Relayed));
}

TEST_CASE("WorldBroadcaster: the order rate limit acks once per window, not once per packet",
          "[world_broadcaster][wingman]") {
    // An ack for every rejected packet would turn a flood into an amplifier pointed at the sender.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster* bcPtr = nullptr; // the spawner calls back in; set below
    fl::WorldQueries q_broadcaster;
    fl::FormationId theFlight = fl::kNoFormation;
    fl::WorldBroadcasterHooks h_broadcaster;
    q_broadcaster.flightSpawner = [&](uint32_t peerId, fl::EntityId lead) {
        theFlight = bcPtr->formations().create("Viper", lead, peerId);
        fl::EntityTransform t{};
        fl::FormationMember m{};
        m.id = em.spawn("builtin:debug-entity", t);
        bcPtr->formations().addMember(theFlight, m);
        return theFlight;
    };
    h_broadcaster.comms.flightOrders = [&](const fl::Formation&, const fl::FormationMember&, uint8_t, fl::EntityId) {
        return true;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster),
                                     std::move(h_broadcaster));
    bcPtr = &broadcaster;
    broadcaster.setFlightCommandRateLimit(2);

    connectPilotPeer(broadcaster, net, 0u);
    net.perPeerSends.clear();

    for (uint32_t i = 0; i < 10; ++i) {
        const auto order = makeOrder(kRejoin, theFlight, fl::kFlightAll, /*seq=*/i + 1);
        broadcaster.onReceive(0u, &order, sizeof(order));
    }

    const auto acks = acksFor(net, 0);
    int rateLimited = 0;
    for (const auto& a : acks) {
        if (a.result == static_cast<uint8_t>(fl::WingmanResult::RateLimited))
            ++rateLimited;
    }
    CHECK(rateLimited == 1); // exactly one, for the whole window
}

TEST_CASE("WorldBroadcaster: a truncated or mis-versioned order is discarded", "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    int handlerCalls = 0;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.comms.flightOrders = [&](const fl::Formation&, const fl::FormationMember&, uint8_t, fl::EntityId) {
        ++handlerCalls;
        return true;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));
    connectPilotPeer(broadcaster, net, 0u);
    net.perPeerSends.clear();

    // Truncated: one byte of msgId and nothing else.
    const uint8_t stub = static_cast<uint8_t>(fl::MsgId::WingmanCommand);
    broadcaster.onReceive(0u, &stub, sizeof(stub));

    // Wrong protocol version.
    auto bad = makeOrder(kRejoin, 1);
    bad.protocolVersion = fl::kProtocolVersion + 1;
    broadcaster.onReceive(0u, &bad, sizeof(bad));

    CHECK(handlerCalls == 0);
    CHECK(acksFor(net, 0).empty()); // neither is worth answering
}

TEST_CASE("WorldBroadcaster: disconnect tears the peer's flight down", "[world_broadcaster][wingman]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster* bcPtr = nullptr; // the spawner calls back in; set below
    fl::WorldQueries q_broadcaster;
    q_broadcaster.flightSpawner = [&](uint32_t peerId, fl::EntityId lead) {
        const fl::FormationId fid = bcPtr->formations().create("Viper", lead, peerId);
        fl::EntityTransform t{};
        fl::FormationMember m{};
        m.id = em.spawn("builtin:debug-entity", t);
        bcPtr->formations().addMember(fid, m);
        return fid;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));
    bcPtr = &broadcaster;

    connectPilotPeer(broadcaster, net, 0u);
    CHECK(broadcaster.formations().size() == 1u);

    broadcaster.onTick(1.0 / 60.0, 1u); // EntityManager::liveCount() only refreshes on tick
    const uint32_t before = em.liveCount();
    CHECK(before == 2u); // the player, plus their one AI wingman

    broadcaster.onDisconnect(0u);

    // The formation and its AI aircraft go with the player: they existed to fly on them, and leaving
    // them holding station on a dead anchor would leak an entity + a controller per disconnect.
    CHECK(broadcaster.formations().size() == 0u);
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK(em.liveCount() == 0u); // both gone
}

// ---------------------------------------------------------------------------
// Combat scoring, the kill feed, and damage penalties (#626)
// ---------------------------------------------------------------------------

namespace {} // namespace

TEST_CASE("WorldBroadcaster: losing your aircraft is a loss on your stats", "[world_broadcaster][combat]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    em.addEventHandler(&broadcaster);
    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);

    em.applyDamage({ack.assignedEntityIdx, ack.assignedEntityGen}, 200.f, fl::EntityId::null());
    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto stats = lastStatsFor(net, 0u);
    REQUIRE(stats.has_value());
    CHECK(stats->a == 0u);
    CHECK(stats->b == 1u);
}

TEST_CASE("WorldBroadcaster: DamageLevelChanged applies the DamageDef penalties to the integrator",
          "[world_broadcaster][combat]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;

    fl::EntityDef def = makeDebugDef();
    fl::DamageDef dmg;
    dmg.light.hpFraction = 0.7f;
    dmg.light.thrustFactor = 0.5f;
    dmg.light.controlFactor = 0.6f;
    dmg.heavy.hpFraction = 0.35f;
    dmg.critical.hpFraction = 0.1f;
    dmg.critical.thrustFactor = 0.1f;
    dmg.critical.controlFactor = 0.2f;
    dmg.critical.avionicsFailure = true;
    def.damage = dmg;
    registry.registerType(def);

    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    em.addEventHandler(&broadcaster);
    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityId player{ack.assignedEntityIdx, ack.assignedEntityGen};

    const fl::FlightIntegrator* fi = broadcaster.integratorFor(player.index);
    REQUIRE(fi != nullptr);
    CHECK(fi->damageThrustFactor() == 1.f);

    em.applyDamage(player, 40.f, fl::EntityId::null()); // 60/100 -> Light
    CHECK(fi->damageThrustFactor() == 0.5f);
    CHECK(fi->damageControlFactor() == 0.6f);

    em.applyDamage(player, 55.f, fl::EntityId::null()); // 5/100 -> Critical (avionics gone)
    CHECK(fi->damageThrustFactor() == 0.1f);
    CHECK(fi->damageControlFactor() == 0.2f);
}

TEST_CASE("WorldBroadcaster: applyWarheadAt damages through the pipeline and EMPs on nuclear",
          "[world_broadcaster][combat]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    em.addEventHandler(&broadcaster);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u); // builds the spatial index the warhead queries

    const auto ack = parseSendAck(net);
    const fl::EntityId player{ack.assignedEntityIdx, ack.assignedEntityGen};
    const fl::EntityState* ps = em.get(player);
    REQUIRE(ps != nullptr);

    // Detonate a nuclear warhead just outside blast range but inside the EMP ring.
    double pos[3] = {ps->transform.pos[0] + 200.0, ps->transform.pos[1], ps->transform.pos[2]};
    fl::BlastSpec nuke{100.f, 80.f, true};
    const auto r = broadcaster.applyWarheadAt(pos, nuke, fl::EntityId::null());

    CHECK(em.get(player)->hp == 100.f); // no shrapnel at 200 m
    CHECK(r.emped >= 1);                // but the avionics are gone (SensorSystem wiring is live)

    // And inside blast range it hurts.
    double close[3] = {ps->transform.pos[0] + 10.0, ps->transform.pos[1], ps->transform.pos[2]};
    fl::BlastSpec he{100.f, 50.f, false};
    broadcaster.applyWarheadAt(close, he, fl::EntityId::null());
    CHECK(em.get(player)->hp < 100.f);
}

// ---------------------------------------------------------------------------
// The fire path (#625): trigger bit -> weapons pass -> hitscan/projectile -> effects TLV
// ---------------------------------------------------------------------------

namespace {

const char* kFpGunToml = R"toml(
[weapon]
id       = "fp:gun"
name     = "FP Gun"
type     = "gun"
category = "air-to-air"
[performance]
max_range_nm     = 0.6
rate_of_fire_rpm = 1200
[warhead]
blast_radius_ft = 3
damage          = 8
[load]
weight_lb   = 300
drag_factor = 0
rounds      = 50
)toml";

const char* kFpMissileToml = R"toml(
[weapon]
id       = "fp:aim"
name     = "FP Missile"
type     = "missile"
category = "air-to-air"
[seeker]
type            = "ir"
sensor_id       = "fp:seeker"
fire_and_forget = true
[performance]
max_range_nm      = 9
min_range_nm      = 0.3
max_speed_kts     = 1500
motor_burn_time_s = 5
max_g             = 20
[warhead]
blast_radius_ft = 30
damage          = 60
[load]
weight_lb   = 190
drag_factor = 0.001
)toml";

// The debug entity, armed: a gun on station 0 and one missile on station 1 (the C8 sandbox arming,
// in miniature). Same type id, so onConnect's spawn picks it up.
fl::EntityDef makeArmedDebugDef() {
    fl::EntityDef def = makeDebugDef();
    fl::Hardpoint gun;
    gun.slot = 0;
    gun.allowed = {"fp:gun"};
    gun.defaultWeapon = "fp:gun";
    fl::Hardpoint rail;
    rail.slot = 1;
    rail.allowed = {"fp:aim"};
    rail.defaultWeapon = "fp:aim";
    def.hardpoints = {gun, rail};
    return def;
}

struct DecodedEffect {
    uint8_t type{0};
    uint8_t weaponClass{0};
    uint32_t srcIdx{0};
    uint32_t tgtIdx{0};
    float pos[3]{};
};

// Read the SnapshotEffects TLV (packed 22-byte records, unaligned) from a snapshot packet.
std::vector<DecodedEffect> decodeEffects(const std::vector<uint8_t>& pkt) {
    std::vector<DecodedEffect> out;
    fl::MsgWorldSnapshotHeader hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset =
        sizeof(hdr) + static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    if (pkt.size() <= extOffset)
        return out;
    uint16_t valueLen{};
    const uint8_t* p = fl::findExt(pkt.data() + extOffset, pkt.size() - extOffset,
                                   static_cast<uint16_t>(fl::ExtTag::SnapshotEffects), valueLen);
    if (!p)
        return out;
    for (uint16_t i = 0; i + fl::kEffectRecordBytes <= valueLen; i += fl::kEffectRecordBytes) {
        DecodedEffect e;
        e.type = p[i + 0];
        e.weaponClass = p[i + 1];
        std::memcpy(&e.srcIdx, p + i + 2, 4u);
        std::memcpy(&e.tgtIdx, p + i + 6, 4u);
        std::memcpy(e.pos, p + i + 10, 12u);
        out.push_back(e);
    }
    return out;
}

bool hasEffect(const std::vector<DecodedEffect>& fx, fl::EffectType t) {
    return std::any_of(fx.begin(), fx.end(), [&](const DecodedEffect& e) { return e.type == static_cast<uint8_t>(t); });
}

} // namespace

TEST_CASE("WorldBroadcaster: the trigger bit fires the gun -- damage lands and effects replicate",
          "[world_broadcaster][firepath]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeArmedDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WeaponRegistry weapons;
    weapons.registerWeapon(fl::parseWeaponDef(kFpGunToml));
    weapons.registerWeapon(fl::parseWeaponDef(kFpMissileToml));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);
    connectPilotPeer(broadcaster, net, 0u); // spawns the armed peer, identity orientation: bore = +X
    const auto ack = parseSendAck(net);
    const fl::EntityState* shooter = em.get({ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(shooter != nullptr);

    // A victim dead ahead of wherever the peer actually spawned, well inside the gun's reach and
    // its 8 m hit radius.
    fl::EntityTransform vt{};
    vt.pos[0] = shooter->transform.pos[0] + 100.0;
    vt.pos[1] = shooter->transform.pos[1];
    vt.pos[2] = shooter->transform.pos[2];
    vt.quat[3] = 1.f;
    const fl::EntityId victim = em.spawn("builtin:debug-entity", vt);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x01u; // gun trigger
    inp.selectedStation = 255u;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    const fl::EntityState* vs = em.get(victim);
    REQUIRE(vs != nullptr);
    CHECK(vs->hp == 92.f); // one 8-damage round connected

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto fx = decodeEffects(snapshotsFor(net, 0).back());
    CHECK(hasEffect(fx, fl::EffectType::WeaponFired)); // tracer at the muzzle
    CHECK(hasEffect(fx, fl::EffectType::Impact));      // and the hit itself
}

TEST_CASE("WorldBroadcaster: store release spawns ONE replicated projectile; stale repeat never re-fires",
          "[world_broadcaster][firepath]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeArmedDebugDef());

    fl::WeaponRegistry weapons;
    weapons.registerWeapon(fl::parseWeaponDef(kFpGunToml));
    const uint32_t aimIdx = weapons.registerWeapon(fl::parseWeaponDef(kFpMissileToml));

    // The projectile's pooled entity type, registered up front because MsgEntityTypeDef only travels
    // in ConnectAck (the ContentBootstrap startup job, in miniature).
    fl::EntityDef proj;
    proj.id = fl::projectileTypeId(*weapons.byIndex(aimIdx));
    proj.name = "proj";
    proj.category = fl::ObjectCategory::Projectile;
    proj.maxHp = 1.f;
    const uint32_t projType = registry.registerType(proj);

    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);
    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x04u;        // fire selected store
    inp.selectedStation = 255u; // keep the default selection (the missile rail)
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 2u); // the peer and its missile
    CHECK(hasEffect(decodeEffects(snapshotsFor(net, 0).back()), fl::EffectType::MissileLaunch));

    // No further input arrives: the jitter buffer stale-repeats the last controls with the fire bit
    // masked, and the edge detector never sees a new press. One press, one missile.
    for (uint64_t t = 2; t <= 5; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK(em.liveCount() == 2u);

    // The projectile replicates like any entity: some snapshot after the spawn carries a FULL record
    // with its type index.
    bool sawProjectile = false;
    for (const auto& pkt : snapshotsFor(net, 0)) {
        for (const DecodedEntity& e : decodeEntities(pkt)) {
            if (e.isFull && e.typeIndex == projType)
                sawProjectile = true;
        }
    }
    CHECK(sawProjectile);
}

TEST_CASE("WorldBroadcaster: a delayed player's gun hits where the target WAS -- lag compensation",
          "[world_broadcaster][firepath][lagcomp]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeArmedDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WeaponRegistry weapons;
    weapons.registerWeapon(fl::parseWeaponDef(kFpGunToml));
    weapons.registerWeapon(fl::parseWeaponDef(kFpMissileToml));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);
    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityState* shooter = em.get({ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(shooter != nullptr);

    // The victim sits dead ahead on the bore for ticks 1..10 -- that world is what the history
    // ring records and what a 5-tick-delayed client is still SEEING.
    fl::EntityTransform vt{};
    vt.pos[0] = shooter->transform.pos[0] + 100.0;
    vt.pos[1] = shooter->transform.pos[1];
    vt.pos[2] = shooter->transform.pos[2];
    vt.quat[3] = 1.f;
    const fl::EntityId victim = em.spawn("builtin:debug-entity", vt);

    for (uint64_t t = 1; t <= 10; ++t)
        broadcaster.onTick(1.0 / 60.0, t);

    // NOW the victim jinks 200 m off the bore. On the server it is nowhere near the ray.
    em.get(victim)->transform.pos[2] += 200.0;

    // The trigger packet echoes snapshot tick 5: estimatedDelayTicks = 10 - 5 = 5, so the hitscan
    // at tick 11 rewinds to tick 6 -- where the victim was still on the bore.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x01u;
    inp.selectedStation = 255u;
    inp.tickIndex = 5u;
    broadcaster.onReceive(0u, &inp, sizeof(inp));
    broadcaster.onTick(1.0 / 60.0, 11u);

    const fl::EntityState* vs = em.get(victim);
    REQUIRE(vs != nullptr);
    CHECK(vs->hp == 92.f); // the shot landed at the rewound position; damage lands on the entity NOW
}

TEST_CASE("WorldBroadcaster: with no delay the same jink is a clean miss -- no free rewind",
          "[world_broadcaster][firepath][lagcomp]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeArmedDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WeaponRegistry weapons;
    weapons.registerWeapon(fl::parseWeaponDef(kFpGunToml));
    weapons.registerWeapon(fl::parseWeaponDef(kFpMissileToml));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);
    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityState* shooter = em.get({ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(shooter != nullptr);

    fl::EntityTransform vt{};
    vt.pos[0] = shooter->transform.pos[0] + 100.0;
    vt.pos[1] = shooter->transform.pos[1];
    vt.pos[2] = shooter->transform.pos[2];
    vt.quat[3] = 1.f;
    const fl::EntityId victim = em.spawn("builtin:debug-entity", vt);

    for (uint64_t t = 1; t <= 10; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    em.get(victim)->transform.pos[2] += 200.0;

    // Same shot, but the packet echoes tick 10: estimatedDelayTicks = 0, rewind 0 -- the ray is
    // tested against the CURRENT (jinked) position and misses.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x01u;
    inp.selectedStation = 255u;
    inp.tickIndex = 10u;
    broadcaster.onReceive(0u, &inp, sizeof(inp));
    broadcaster.onTick(1.0 / 60.0, 11u);

    const fl::EntityState* vs = em.get(victim);
    REQUIRE(vs != nullptr);
    CHECK(vs->hp == 100.f);
}

TEST_CASE("WorldBroadcaster: the zero-pack sandbox peer spawns ARMED and the cannon fires",
          "[world_broadcaster][firepath][sandbox]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(fl::builtinDebugEntityDef()); // the REAL sandbox def (#440), not a fixture

    fl::WeaponRegistry weapons;
    REQUIRE(fl::registerBuiltinWeapons(weapons) == 8u);

    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);
    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityState* shooter = em.get({ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(shooter != nullptr);

    fl::EntityTransform vt{};
    vt.pos[0] = shooter->transform.pos[0] + 300.0; // well inside the cannon's 1200 m reach
    vt.pos[1] = shooter->transform.pos[1];
    vt.pos[2] = shooter->transform.pos[2];
    vt.quat[3] = 1.f;
    const fl::EntityId victim = em.spawn("builtin:debug-entity", vt);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x01u; // gun trigger — station selection untouched: the gun needs none
    inp.selectedStation = 255u;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    const fl::EntityState* vs = em.get(victim);
    REQUIRE(vs != nullptr);
    CHECK(vs->hp == 100.f - fl::BuiltinWeapon::cannon().warhead.damage);
    CHECK(hasEffect(decodeEffects(snapshotsFor(net, 0).back()), fl::EffectType::WeaponFired));

    // And the default selection is a rail, not the gun — "selected" means the stores.
    const fl::LoadoutState ls = fl::buildLoadout(fl::builtinDebugEntityDef(), weapons);
    CHECK(ls.stations.size() == 8u);
    CHECK(ls.selected == 1u);                        // first non-gun rail (IR)
    CHECK(ls.stations[6].weaponIndex == UINT32_MAX); // the drop tank (Fuel) is inert — no firing station
    CHECK(ls.stations[7].weaponIndex == UINT32_MAX); // the sensor pod (Pod) is inert too
}

TEST_CASE("WorldBroadcaster: a designated IR missile launch flies out and kills through the pipeline",
          "[world_broadcaster][firepath][seeker]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(fl::builtinDebugEntityDef());

    fl::WeaponRegistry weapons;
    fl::registerBuiltinWeapons(weapons);

    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_broadcaster;
    q_broadcaster.sensorDefs = [](const std::string& id) -> std::shared_ptr<const fl::sensor::SensorDef> {
        if (id == "builtin:ir-seeker")
            return {std::shared_ptr<const fl::sensor::SensorDef>{}, &fl::sensor::BuiltinSensors::irSeeker()};
        return nullptr;
    };
    // Declared above the queries and captured BY REFERENCE: the designator is frozen at construction
    // now, but it is not called until the shot, by which point the spawn below has filled this in.
    // The designator is frozen at construction now, but this test installed it only AFTER the victim
    // existed -- and when it is consulted matters, because a designation is latched. `armed` puts the
    // switch-on back exactly where it was: the query is present from the start and answers "no target"
    // until the test arms it. A default-constructed EntityId would NOT do: that is index 0, a real
    // entity.
    fl::EntityId victim = fl::EntityId::null();
    bool designatorArmed = false;
    q_broadcaster.targetDesignator = [&](const fl::EntityState&, const float*) -> fl::EntityId {
        return designatorArmed ? victim : fl::EntityId::null();
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));
    em.addEventHandler(&broadcaster);
    broadcaster.setWeaponRegistry(&weapons);
    // Missiles get projectile entity types the same way the server registers them at startup.
    fl::registerProjectileEntityDefs(weapons, registry, logger);
    // Seeker heads resolve to the compiled-in defs (the fl-server resolver does the same, #440).

    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityState* shooter = em.get({ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(shooter != nullptr);

    // A hostile 2 km dead ahead. The designator is the #610 seam: here a test lambda standing in
    // for fl-server's contact-honest one.
    fl::EntityTransform vt{};
    vt.pos[0] = shooter->transform.pos[0] + 2000.0;
    vt.pos[1] = shooter->transform.pos[1];
    vt.pos[2] = shooter->transform.pos[2];
    vt.quat[3] = 1.f;
    victim = em.spawn("builtin:debug-entity", vt);
    designatorArmed = true;

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x04u;        // fire selected store
    inp.selectedStation = 255u; // default selection = the first IR rail
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Fly the engagement out. 2 km at missile speeds is a few seconds; give it ten.
    bool victimHit = false;
    for (uint64_t t = 1; t <= 600 && !victimHit; ++t) {
        broadcaster.onTick(1.0 / 60.0, t);
        const fl::EntityState* vs = em.get(victim);
        victimHit = !vs || vs->dead || vs->hp < 100.f;
    }
    CHECK(victimHit); // the warhead connected through the same damage pipeline as everything else
}

TEST_CASE("WorldBroadcaster: every builtin flying store releases from the zero-pack debug entity (#862)",
          "[world_broadcaster][firepath][sandbox]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(fl::builtinDebugEntityDef());

    fl::WeaponRegistry weapons;
    fl::registerBuiltinWeapons(weapons);

    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_broadcaster;
    q_broadcaster.sensorDefs = [](const std::string& id) -> std::shared_ptr<const fl::sensor::SensorDef> {
        if (id == "builtin:ir-seeker")
            return {std::shared_ptr<const fl::sensor::SensorDef>{}, &fl::sensor::BuiltinSensors::irSeeker()};
        if (id == "builtin:radar-seeker")
            return {std::shared_ptr<const fl::sensor::SensorDef>{}, &fl::sensor::BuiltinSensors::radarSeeker()};
        if (id == "builtin:sarh-seeker")
            return {std::shared_ptr<const fl::sensor::SensorDef>{}, &fl::sensor::BuiltinSensors::sarhSeeker()};
        return nullptr;
    };
    // Declared above the queries and captured BY REFERENCE: the designator is frozen at construction
    // now, but it is not called until the shot, by which point the spawn below has filled this in.
    // The designator is frozen at construction now, but this test installed it only AFTER the victim
    // existed -- and when it is consulted matters, because a designation is latched. `armed` puts the
    // switch-on back exactly where it was: the query is present from the start and answers "no target"
    // until the test arms it. A default-constructed EntityId would NOT do: that is index 0, a real
    // entity.
    fl::EntityId victim = fl::EntityId::null();
    bool designatorArmed = false;
    q_broadcaster.targetDesignator = [&](const fl::EntityState&, const float*) -> fl::EntityId {
        return designatorArmed ? victim : fl::EntityId::null();
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));
    em.addEventHandler(&broadcaster);
    broadcaster.setWeaponRegistry(&weapons);
    fl::registerProjectileEntityDefs(weapons, registry, logger);
    // Resolve every builtin seeker head like fl-server does at startup.

    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityState* shooter = em.get({ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(shooter != nullptr);

    // A target 2 km ahead so the designated shots (SARH) have something to support against.
    fl::EntityTransform vt{};
    vt.pos[0] = shooter->transform.pos[0] + 2000.0;
    vt.pos[1] = shooter->transform.pos[1];
    vt.pos[2] = shooter->transform.pos[2];
    vt.quat[3] = 1.f;
    victim = em.spawn("builtin:debug-entity", vt);
    designatorArmed = true;

    // Count live entities of a given projectile type currently in the world.
    auto projectileCount = [&](const char* id) {
        const uint32_t typeIdx = registry.indexById(id);
        int n = 0;
        em.forEach([&](const fl::EntityState& e) {
            if (!e.dead && e.typeIndex == typeIdx)
                ++n;
        });
        return n;
    };

    // A real client feeds a fresh MsgClientInput every tick with a strictly increasing seqNum — a
    // stale/duplicate seqNum is discarded by the staleness guard, so a single re-sent packet never
    // fires twice. Drive the peer that way: one input per tick, seqNum climbing, station held.
    uint16_t seq = 0;
    uint64_t tick = 0;
    auto tickInput = [&](uint8_t buttons, uint8_t station) {
        fl::MsgClientInput inp{};
        inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
        inp.protocolVersion = fl::kProtocolVersion;
        inp.seqNum = ++seq;
        inp.buttons = buttons;
        inp.selectedStation = station;
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, ++tick);
    };

    // Fire each flying store from its station in turn; return the PEAK count of `projId` seen while
    // the trigger is held (before the round flies out of the world). Station indices follow
    // builtinDebugEntityDef()'s order: 0 gun, 1 IR, 2 radar, 3 SARH, 4 bomb, 5 rocket, 6 drop tank.
    auto fireStation = [&](uint8_t station, const char* projId) {
        int peak = 0;
        for (int k = 0; k < 12; ++k) { // hold: single stores fire on the edge, a rocket pod ripples
            tickInput(0x04u, station);
            peak = std::max(peak, projectileCount(projId));
        }
        // Release + run out kReleaseCooldownTicks (30) so the NEXT store sees a fresh, uncooled edge.
        for (int k = 0; k < 31; ++k)
            tickInput(0x00u, station);
        return peak;
    };

    CHECK(fireStation(4u, "projectile:builtin:bomb") >= 1);         // bomb
    CHECK(fireStation(5u, "projectile:builtin:rocket") >= 1);       // rocket pod (ripples)
    CHECK(fireStation(3u, "projectile:builtin:sarh-missile") >= 1); // SARH missile
}

TEST_CASE("WorldBroadcaster: the pre-launch LOCK cue reaches the own record's weaponFlags",
          "[world_broadcaster][firepath][seeker]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(fl::builtinDebugEntityDef());

    fl::WeaponRegistry weapons;
    fl::registerBuiltinWeapons(weapons);

    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_broadcaster;
    q_broadcaster.sensorDefs = [](const std::string& id) -> std::shared_ptr<const fl::sensor::SensorDef> {
        if (id == "builtin:ir-seeker")
            return {std::shared_ptr<const fl::sensor::SensorDef>{}, &fl::sensor::BuiltinSensors::irSeeker()};
        return nullptr;
    };
    // Declared above the queries and captured BY REFERENCE: the designator is frozen at construction
    // now, but it is not called until the shot, by which point the spawn below has filled this in.
    // The designator is frozen at construction now, but this test installed it only AFTER the victim
    // existed -- and when it is consulted matters, because a designation is latched. `armed` puts the
    // switch-on back exactly where it was: the query is present from the start and answers "no target"
    // until the test arms it. A default-constructed EntityId would NOT do: that is index 0, a real
    // entity.
    fl::EntityId victim = fl::EntityId::null();
    bool designatorArmed = false;
    q_broadcaster.targetDesignator = [&](const fl::EntityState&, const float*) -> fl::EntityId {
        return designatorArmed ? victim : fl::EntityId::null();
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));
    broadcaster.setWeaponRegistry(&weapons);

    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityState* shooter = em.get({ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(shooter != nullptr);

    // A target 2 km dead ahead, inside the IR head's acquisition envelope; the designator points
    // at it. The default selection is the first IR rail, so the growl should come up.
    fl::EntityTransform vt{};
    vt.pos[0] = shooter->transform.pos[0] + 2000.0;
    vt.pos[1] = shooter->transform.pos[1];
    vt.pos[2] = shooter->transform.pos[2];
    vt.quat[3] = 1.f;
    victim = em.spawn("builtin:debug-entity", vt);
    designatorArmed = true;

    // Run past a full cue cadence window (the cue is computed at ~10 Hz per peer).
    for (uint64_t t = 1; t <= 12; ++t)
        broadcaster.onTick(1.0 / 60.0, t);

    bool sawLock = false;
    for (const auto& pkt : snapshotsFor(net, 0)) {
        for (const DecodedEntity& e : decodeEntities(pkt)) {
            if (e.hasLoadout && (e.weaponFlags & 0x01u))
                sawLock = true;
        }
    }
    CHECK(sawLock);
}

// ---------------------------------------------------------------------------
// Entity-entity collision phase (#630)
// ---------------------------------------------------------------------------

namespace {
// Spawn two entities overlapping (both at ~the same point) with opposing velocities so the
// relative speed is lethal, and step one tick. Returns the pair's post-collision HP.
struct CollisionOutcome {
    float hpA{-1.f};
    float hpB{-1.f};
};
CollisionOutcome runCollisionCase(bool crashDamage, double separationM, float speedMps, fl::JobSystem* jobs) {
    static MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef()); // AirVehicle, 100 hp, 8 m default collision radius
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    if (jobs)
        broadcaster.setJobSystem(*jobs);
    fl::DamageRules rules;
    rules.crashDamage = crashDamage;
    broadcaster.setDamageRules(rules);

    fl::EntityTransform ta{};
    ta.pos[1] = 5000.0;
    ta.vel[0] = speedMps; // closing from the left
    ta.quat[3] = 1.f;
    const fl::EntityId a = em.spawn("builtin:debug-entity", ta);

    fl::EntityTransform tb{};
    tb.pos[0] = separationM;
    tb.pos[1] = 5000.0;
    tb.vel[0] = -speedMps; // closing from the right
    tb.quat[3] = 1.f;
    const fl::EntityId b = em.spawn("builtin:debug-entity", tb);

    broadcaster.onTick(1.0 / 60.0, 1u);

    CollisionOutcome out;
    if (const fl::EntityState* sa = em.get(a))
        out.hpA = sa->dead ? 0.f : sa->hp;
    if (const fl::EntityState* sb = em.get(b))
        out.hpB = sb->dead ? 0.f : sb->hp;
    return out;
}
} // namespace

TEST_CASE("WorldBroadcaster: two overlapping entities collide and BOTH take relative-speed damage",
          "[world_broadcaster][collision]") {
    // 10 m apart, radii 8 + 8 = 16 > 10: overlapping. 300 m/s each = 600 m/s closure — lethal.
    const auto out = runCollisionCase(/*crashDamage=*/true, /*separation=*/10.0, /*speed=*/300.f, nullptr);
    CHECK(out.hpA < 100.f);
    CHECK(out.hpB < 100.f);
    CHECK(out.hpA == out.hpB); // symmetric: a mid-air hurts both sides equally
}

TEST_CASE("WorldBroadcaster: a gentle brush below the threshold does no damage", "[world_broadcaster][collision]") {
    const auto out = runCollisionCase(/*crashDamage=*/true, /*separation=*/10.0, /*speed=*/1.f, nullptr);
    CHECK(out.hpA == 100.f); // 2 m/s closure is under the free-brush threshold
    CHECK(out.hpB == 100.f);
}

TEST_CASE("WorldBroadcaster: entities clear of each other never collide", "[world_broadcaster][collision]") {
    // 100 m apart, radii 8 + 8 = 16 < 100: no overlap, even at lethal closing speed.
    const auto out = runCollisionCase(/*crashDamage=*/true, /*separation=*/100.0, /*speed=*/300.f, nullptr);
    CHECK(out.hpA == 100.f);
    CHECK(out.hpB == 100.f);
}

TEST_CASE("WorldBroadcaster: crashDamage=false gates entity collisions off", "[world_broadcaster][collision]") {
    const auto out = runCollisionCase(/*crashDamage=*/false, /*separation=*/10.0, /*speed=*/300.f, nullptr);
    CHECK(out.hpA == 100.f); // detected, but the difficulty gate suppresses the damage
    CHECK(out.hpB == 100.f);
}

TEST_CASE("WorldBroadcaster: collision detection is serial-equivalent across worker counts",
          "[world_broadcaster][collision]") {
    CollisionOutcome ref;
    for (uint32_t total : {1u, 2u, 8u}) {
        fl::JobSystem jobs(total);
        const auto out = runCollisionCase(/*crashDamage=*/true, /*separation=*/10.0, /*speed=*/300.f, &jobs);
        if (total == 1u)
            ref = out;
        else {
            REQUIRE(out.hpA == ref.hpA); // byte-identical damage regardless of worker count
            REQUIRE(out.hpB == ref.hpB);
        }
    }
}

// ---------------------------------------------------------------------------
// Per-subsystem damage (#675)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: a warhead that fails an engine subsystem shows on the wire flags",
          "[world_broadcaster][subsystem]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;

    // A twin whose ONLY subsystems are the two engines (low HP, so one blast fails one). The pick
    // is therefore deterministically an engine, and its failure must reach the integrator's
    // engineFailFlags (which the snapshot already replicates).
    fl::EntityDef def = makeDebugDef("test:twin");
    fl::DamageDef dmg;
    dmg.light.hpFraction = 0.7f;
    dmg.heavy.hpFraction = 0.35f;
    dmg.critical.hpFraction = 0.1f;
    fl::SubsystemSet subs;
    subs.parts[static_cast<int>(fl::Subsystem::EngineLeft)] = {10.f, 1.f};
    subs.parts[static_cast<int>(fl::Subsystem::EngineRight)] = {10.f, 1.f};
    dmg.subsystems = subs;
    def.damage = dmg;
    def.maxHp = 1000.f; // survives the blast so we see the ENGINE failure, not death
    registry.registerType(def);

    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::EntityTransform t{};
    t.pos[1] = 5000.0;
    t.quat[3] = 1.f;
    const fl::EntityId victim = em.spawn("test:twin", t);
    broadcaster.registerController(victim,
                                   std::make_unique<ConstantController>()); // gives it an integrator + subsystems

    broadcaster.onTick(1.0 / 60.0, 1u); // build the spatial index + integrator

    const fl::EntityState* vs = em.get(victim);
    REQUIRE(vs != nullptr);
    double pos[3] = {vs->transform.pos[0] + 3.0, vs->transform.pos[1], vs->transform.pos[2]};
    fl::BlastSpec blast{20.f, 60.f, false}; // > the 10 hp engine pool
    broadcaster.applyWarheadAt(pos, blast, fl::EntityId::null());

    const fl::FlightIntegrator* fi = broadcaster.integratorFor(victim.index);
    REQUIRE(fi != nullptr);
    CHECK((fi->engineFailFlags() & (fl::kEngineFailLeft | fl::kEngineFailRight)) != 0);
}

TEST_CASE("WorldBroadcaster: the builtin debug entity walks through damage levels + a subsystem failure (#864)",
          "[world_broadcaster][subsystem][sandbox]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(fl::builtinDebugEntityDef()); // the REAL sandbox def, now with a damage model
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::EntityTransform t{};
    t.pos[1] = 5000.0;
    t.quat[3] = 1.f;
    const fl::EntityId victim = em.spawn("builtin:debug-entity", t);
    // Spawn stationary (initialAirspeed 0, #883) so it stays put as a damage dummy — the walk below
    // blasts at its position between manual onTick() calls, and a cruising victim would drift a tick
    // ahead of the (once-per-tick) spatial index the warhead queries.
    broadcaster.registerController(victim, std::make_unique<ConstantController>(), nullptr, /*airspeed=*/0.f);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto levelOf = [&]() -> int {
        const fl::EntityState* s = em.get(victim);
        return s ? static_cast<int>(s->damageLevel) : static_cast<int>(fl::DamageLevel::Destroyed);
    };
    auto blastAtVictim = [&](float dmg, uint64_t tick) {
        const fl::EntityState* s = em.get(victim);
        REQUIRE(s != nullptr);
        double pos[3] = {s->transform.pos[0], s->transform.pos[1], s->transform.pos[2]};
        broadcaster.applyWarheadAt(pos, fl::BlastSpec{6.f, dmg, false}, fl::EntityId::null());
        broadcaster.onTick(1.0 / 60.0, tick);
    };

    CHECK(levelOf() == static_cast<int>(fl::DamageLevel::Intact));

    // maxHp = 100; each blast is full damage at the centre. Walk the thresholds (light 0.66 / heavy
    // 0.33 / critical 0.12) before death.
    blastAtVictim(40.f, 2u); // hp 60 -> Light
    CHECK(levelOf() == static_cast<int>(fl::DamageLevel::Light));
    blastAtVictim(30.f, 3u); // hp 30 -> Heavy
    CHECK(levelOf() == static_cast<int>(fl::DamageLevel::Heavy));
    blastAtVictim(20.f, 4u); // hp 10 -> Critical
    CHECK(levelOf() == static_cast<int>(fl::DamageLevel::Critical));
    blastAtVictim(20.f, 5u); // hp 0 -> dead
    CHECK(em.get(victim) == nullptr);
}

TEST_CASE("WorldBroadcaster: directed hits on the builtin debug entity fail its subsystems (#864)",
          "[world_broadcaster][subsystem][sandbox]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(fl::builtinDebugEntityDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::EntityTransform t{};
    t.pos[1] = 5000.0;
    t.quat[3] = 1.f; // nose +X, right +Z
    const fl::EntityId victim = em.spawn("builtin:debug-entity", t);
    broadcaster.registerController(victim, std::make_unique<ConstantController>());
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Boost HP so the airframe survives a barrage — we are testing SUBSYSTEM failure, not death. Each
    // blast lands from behind-and-right (the shrapnel travels forward → the directional-bias favors
    // the engines, #675). The four non-engine pools exhaust and are removed from the pick, so within a
    // handful of directed hits an engine has to be the one that fails.
    if (fl::EntityState* s = em.get(victim)) {
        s->maxHp = 100000.f;
        s->hp = 100000.f;
    }

    bool engineFailed = false;
    for (uint64_t k = 0; k < 16 && !engineFailed; ++k) {
        const fl::EntityState* s = em.get(victim);
        REQUIRE(s != nullptr);
        double pos[3] = {s->transform.pos[0] - 3.0, s->transform.pos[1], s->transform.pos[2] + 1.0}; // behind-right
        broadcaster.applyWarheadAt(pos, fl::BlastSpec{12.f, 200.f, false}, fl::EntityId::null());
        broadcaster.onTick(1.0 / 60.0, 2u + k);
        if (const fl::FlightIntegrator* fi = broadcaster.integratorFor(victim.index))
            engineFailed = (fi->engineFailFlags() & (fl::kEngineFailLeft | fl::kEngineFailRight)) != 0;
    }
    CHECK(engineFailed); // a subsystem hit reached the engines and asymmetric thrust shows on the integrator
}

TEST_CASE("WorldBroadcaster: an entity without a subsystems table is unaffected by the router",
          "[world_broadcaster][subsystem]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef()); // no [damage.subsystems]

    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityId player{ack.assignedEntityIdx, ack.assignedEntityGen};
    broadcaster.onTick(1.0 / 60.0, 1u);

    const fl::EntityState* ps = em.get(player);
    REQUIRE(ps != nullptr);
    double pos[3] = {ps->transform.pos[0] + 3.0, ps->transform.pos[1], ps->transform.pos[2]};
    broadcaster.applyWarheadAt(pos, fl::BlastSpec{20.f, 40.f, false}, fl::EntityId::null());

    // No subsystem model → no engine-out flags, ever (the 3-level tier model is untouched).
    const fl::FlightIntegrator* fi = broadcaster.integratorFor(player.index);
    REQUIRE(fi != nullptr);
    CHECK((fi->engineFailFlags() & (fl::kEngineFailLeft | fl::kEngineFailRight)) == 0);
}

// ---------------------------------------------------------------------------
// Mission player slots (#854) — a connecting pilot binds to a slot
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: a pilot beyond the slots falls back to the default spawn", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    registry.registerType(makeDebugDef("mission:fighter"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setPlayerFaction(7); // the default-path faction
    broadcaster.setSpawnPoints({{9000.0, 100.0, 9000.0}});

    fl::WorldBroadcaster::MissionSpawnSlot slot;
    slot.entityType = "mission:fighter";
    slot.factionIndex = 2;
    slot.pos[0] = 500.0;
    slot.pos[1] = 750.0;
    slot.pos[2] = -250.0;
    broadcaster.setMissionPlayerSlots({slot});

    connectPilotPeer(broadcaster, net, 0u); // claims the only slot
    connectPilotPeer(broadcaster, net, 1u); // slots full -> default spawn point + player faction

    const fl::EntityState* second = slotPeerEntity(em, 1u);
    REQUIRE(second != nullptr);
    CHECK(second->factionIndex == 7);                                       // default-path faction, not the slot's 2
    CHECK(second->transform.pos[0] == 9000.0);                              // default spawn point, not the slot's 500
    CHECK(second->typeIndex == registry.indexById("builtin:debug-entity")); // default type, not mission:fighter
}

// ---------------------------------------------------------------------------
// A mission slot's loadout (#1209) — the fit a mission chooses for the human
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: a pilot takes off with the loadout the mission slot names (#1209)",
          "[world_broadcaster][firepath]") {
    // The training case: a gunnery lesson wants the student on the gun, not holding two missiles.
    // The slot strips the rail, so pulling the store-release trigger produces NOTHING to launch.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeArmedDebugDef());

    fl::WeaponRegistry weapons;
    weapons.registerWeapon(fl::parseWeaponDef(kFpGunToml));
    const uint32_t aimIdx = weapons.registerWeapon(fl::parseWeaponDef(kFpMissileToml));
    fl::EntityDef proj;
    proj.id = fl::projectileTypeId(*weapons.byIndex(aimIdx));
    proj.name = "proj";
    proj.category = fl::ObjectCategory::Projectile;
    proj.maxHp = 1.f;
    registry.registerType(proj);

    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);

    fl::WorldBroadcaster::MissionSpawnSlot slot;
    slot.entityType = "builtin:debug-entity"; // makeArmedDebugDef shares this id
    slot.factionIndex = 1;
    slot.loadout = {"fp:gun", "~"}; // keep the gun, strip the missile rail
    broadcaster.setMissionPlayerSlots({slot});

    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x04u;        // fire the selected store
    inp.selectedStation = 255u; // whatever the loadout selected
    broadcaster.onReceive(0u, &inp, sizeof(inp));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(em.liveCount() == 1u); // the pilot alone: an empty rail has nothing to release
}

TEST_CASE("WorldBroadcaster: a mission slot with no loadout keeps the entity's default fit (#1209)",
          "[world_broadcaster][firepath]") {
    // The same slot without a loadout: the default rail is still armed and still fires. This is what
    // every mission slot did before #1209, and the field defaulting empty is what keeps it that way.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeArmedDebugDef());

    fl::WeaponRegistry weapons;
    weapons.registerWeapon(fl::parseWeaponDef(kFpGunToml));
    const uint32_t aimIdx = weapons.registerWeapon(fl::parseWeaponDef(kFpMissileToml));
    fl::EntityDef proj;
    proj.id = fl::projectileTypeId(*weapons.byIndex(aimIdx));
    proj.name = "proj";
    proj.category = fl::ObjectCategory::Projectile;
    proj.maxHp = 1.f;
    registry.registerType(proj);

    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);

    fl::WorldBroadcaster::MissionSpawnSlot slot;
    slot.entityType = "builtin:debug-entity";
    slot.factionIndex = 1;
    broadcaster.setMissionPlayerSlots({slot});

    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x04u;
    inp.selectedStation = 255u;
    broadcaster.onReceive(0u, &inp, sizeof(inp));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(em.liveCount() == 2u); // the pilot and the missile it just launched
}

TEST_CASE("WorldBroadcaster: setEntityLoadout overrides a controlled entity's stores (#855)",
          "[world_broadcaster][firepath]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeArmedDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WeaponRegistry weapons;
    weapons.registerWeapon(fl::parseWeaponDef(kFpGunToml));
    weapons.registerWeapon(fl::parseWeaponDef(kFpMissileToml));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setWeaponRegistry(&weapons);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    const fl::EntityId id = em.spawn("builtin:debug-entity", t); // makeArmedDebugDef shares this id
    REQUIRE(id.valid());

    std::vector<std::string> warn;
    // No controller yet -> the entity has no ControlledEntity, so the override is refused.
    CHECK_FALSE(broadcaster.setEntityLoadout(id, {"~", "~"}, warn));

    broadcaster.registerController(id, std::make_unique<ConstantController>());

    // A valid override (strip the missile rail, keep the gun) succeeds with no warnings.
    warn.clear();
    CHECK(broadcaster.setEntityLoadout(id, {"fp:gun", "~"}, warn));
    CHECK(warn.empty());

    // A store not allowed on that station: the call still finds the entity (returns true) but records a
    // warning and leaves the station empty. fp:aim is a missile, not allowed on the gun station 0.
    warn.clear();
    CHECK(broadcaster.setEntityLoadout(id, {"fp:aim", "~"}, warn));
    CHECK_FALSE(warn.empty());
}

// --- Crew roster + turret pose replication (#972) --------------------------------------------------

namespace {} // namespace

TEST_CASE("WorldBroadcaster: crewed aircraft replicate roster on connect; single-seat do not (#972)",
          "[world_broadcaster][crew]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef()); // a single-seat entity
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());

    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr; // no gunner bot: the turret rests at az=el=0, which still replicates
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);

    // Spawn an AI crewed bomber near the origin so an observer sees it.
    fl::EntityTransform t{};
    t.pos[1] = 800.0;
    t.quat[3] = 1.f;
    const fl::EntityId bomber = em.spawn("test:crewbomber", t);
    wb.registerController(bomber, std::make_unique<StillCtl>(), nullptr, 0.f);

    // An observer connecting must receive the bomber's MsgCrewRoster (crewed) among its ConnectAck-time
    // reliable sends.
    connectObserverPeer(wb, net, 1u);
    bool sawRoster = false;
    fl::MsgCrewRosterHeader rhdr{};
    for (const auto& pkt : net.sends) {
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::CrewRoster)) {
            REQUIRE(fl::readMsg(pkt.data(), pkt.size(), rhdr));
            if (rhdr.entityIdx == bomber.index) {
                sawRoster = true;
                CHECK(rhdr.seatCount == 2);
                CHECK(rhdr.turretCount == 1);
                fl::CrewRosterSeat s0{};
                REQUIRE(fl::readRecordAt(pkt.data(), pkt.size(), sizeof(rhdr), s0));
                s0.role[sizeof(s0.role) - 1] = '\0';
                CHECK(std::string(s0.role) == "pilot");
            }
        }
    }
    CHECK(sawRoster);

    // Tick once and confirm the observer's snapshot carries a SnapshotCrew TLV naming the bomber, and
    // that the single-seat debug entity (if present) never appears in it.
    clearSnapshots(net);
    wb.onTick(1.0 / 60.0, 1u);
    auto snaps = snapshotsFor(net, 1u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps.back();
    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) +
                                  static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() >= extOffset);
    const auto* ext = pkt.data() + extOffset;
    const auto extSz = pkt.size() - extOffset;
    uint16_t crewLen = 0;
    const uint8_t* cp = fl::findExt(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotCrew), crewLen);
    REQUIRE(cp != nullptr);
    REQUIRE(crewLen >= 6u);
    CHECK(cp[0] == 1u); // exactly one crewed entity in the TLV
    uint32_t crewIdx = 0;
    std::memcpy(&crewIdx, cp + 1, 4);
    CHECK(crewIdx == bomber.index);
    CHECK(cp[5] == 1u); // one turret
}

TEST_CASE("WorldBroadcaster: a single-seat-only world emits no SnapshotCrew TLV (#972)", "[world_broadcaster][crew]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);
    wb.setGroundElevation(0.f);

    connectPilotPeer(wb, net, 0u); // a single-seat pilot aircraft
    clearSnapshots(net);
    wb.onTick(1.0 / 60.0, 1u);
    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps.back();
    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) +
                                  static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    if (pkt.size() > extOffset) {
        uint16_t crewLen = 0;
        const uint8_t* cp = fl::findExt(pkt.data() + extOffset, pkt.size() - extOffset,
                                        static_cast<uint16_t>(fl::ExtTag::SnapshotCrew), crewLen);
        CHECK(cp == nullptr); // no crewed entity -> no crew block (single-seat snapshots unchanged)
    }
}

// --- Seat join / handoff (#974) --------------------------------------------------------------------

namespace {
// Extract every MsgSeatResult sent to a specific peer, in order.
static std::vector<fl::MsgSeatResult> seatResultsFor(const MockNetwork& net, uint32_t peerId) {
    std::vector<fl::MsgSeatResult> out;
    for (const auto& [pid, pkt] : net.perPeerSends) {
        if (pid == peerId && pkt.size() >= sizeof(fl::MsgSeatResult) &&
            pkt[0] == static_cast<uint8_t>(fl::MsgId::SeatResult)) {
            fl::MsgSeatResult r{};
            std::memcpy(&r, pkt.data(), sizeof(r));
            out.push_back(r);
        }
    }
    // perPeerSends may not include reliable sends; also scan net.sends (which records send()).
    for (const auto& pkt : net.sends) {
        if (pkt.size() >= sizeof(fl::MsgSeatResult) && pkt[0] == static_cast<uint8_t>(fl::MsgId::SeatResult)) {
            fl::MsgSeatResult r{};
            std::memcpy(&r, pkt.data(), sizeof(r));
            out.push_back(r);
        }
    }
    return out;
}
} // namespace

TEST_CASE("WorldBroadcaster: a human joins a gunner seat; a second human is denied (#974)",
          "[world_broadcaster][crew]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());
    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr;
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);

    fl::EntityTransform t{};
    t.pos[1] = 800.0;
    t.quat[3] = 1.f;
    const fl::EntityId bomber = em.spawn("test:crewbomber", t);
    wb.registerController(bomber, std::make_unique<StillCtl>(), nullptr, 0.f);

    // Peer 1 connects (spawns its own single-seat aircraft), then requests the bomber's gunner seat.
    connectPilotPeer(wb, net, 1u);
    net.sends.clear();
    net.perPeerSends.clear();
    const fl::MsgSeatRequest r1 = joinReq(bomber, 1);
    wb.onReceive(1u, &r1, sizeof(r1));
    auto res1 = seatResultsFor(net, 1u);
    REQUIRE(!res1.empty());
    CHECK(res1.back().code == static_cast<uint8_t>(fl::SeatResultCode::Granted));
    CHECK(wb.occupantPeerFor(bomber, 1) == 1u); // peer 1 now holds the gunner seat

    // Peer 2 connects and requests the SAME seat -> denied (humans never displace humans).
    connectPilotPeer(wb, net, 2u);
    net.sends.clear();
    net.perPeerSends.clear();
    const fl::MsgSeatRequest r2 = joinReq(bomber, 1);
    wb.onReceive(2u, &r2, sizeof(r2));
    auto res2 = seatResultsFor(net, 2u);
    REQUIRE(!res2.empty());
    CHECK(res2.back().code == static_cast<uint8_t>(fl::SeatResultCode::SeatOccupiedByHuman));
    CHECK(wb.occupantPeerFor(bomber, 1) == 1u); // still peer 1

    // Requesting the Fly seat is denied (it belongs to the owning pilot, not joinable).
    net.sends.clear();
    const fl::MsgSeatRequest rFly = joinReq(bomber, 0);
    wb.onReceive(2u, &rFly, sizeof(rFly));
    auto res3 = seatResultsFor(net, 2u);
    REQUIRE(!res3.empty());
    CHECK(res3.back().code == static_cast<uint8_t>(fl::SeatResultCode::FlySeatNotJoinable));
}

TEST_CASE("WorldBroadcaster: a peer-spawned crewed airframe persists while a human gunner remains (#974)",
          "[world_broadcaster][crew]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());
    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr;
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);

    // Peer 1 spawns and OWNS a crewed bomber (requests that type).
    connectPilotPeer(wb, net, 1u, "test:crewbomber");
    fl::MsgConnectAck ack{};
    // Find peer 1's assigned entity from its ConnectAck.
    for (const auto& pkt : net.sends)
        if (pkt.size() >= sizeof(fl::MsgConnectAck) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            std::memcpy(&ack, pkt.data(), sizeof(ack));
    const fl::EntityId bomber{ack.assignedEntityIdx, ack.assignedEntityGen};
    REQUIRE(bomber.valid());

    // Peer 2 joins the bomber's gunner seat.
    connectPilotPeer(wb, net, 2u);
    const fl::MsgSeatRequest r = joinReq(bomber, 1);
    wb.onReceive(2u, &r, sizeof(r));
    REQUIRE(wb.occupantPeerFor(bomber, 1) == 2u);

    wb.onTick(1.0 / 60.0, 1u);
    REQUIRE(em.get(bomber) != nullptr); // alive

    // The owning pilot (peer 1) disconnects. The airframe must PERSIST (peer 2 still occupies a seat).
    wb.onDisconnect(1u);
    wb.onTick(1.0 / 60.0, 2u);
    CHECK(em.get(bomber) != nullptr);           // NOT destroyed out from under the gunner
    CHECK(wb.occupantPeerFor(bomber, 1) == 2u); // the gunner still holds its seat

    // Now the last human (peer 2) disconnects -> the orphaned peer-spawned airframe is retired.
    wb.onDisconnect(2u);
    wb.onTick(1.0 / 60.0, 3u);
    CHECK(em.get(bomber) == nullptr); // retired once the last human left
}

TEST_CASE("WorldBroadcaster: a peer-spawned single-seat aircraft despawns on pilot disconnect (#974)",
          "[world_broadcaster][crew]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);
    wb.setGroundElevation(0.f);

    connectPilotPeer(wb, net, 1u);
    fl::MsgConnectAck ack{};
    for (const auto& pkt : net.sends)
        if (pkt.size() >= sizeof(fl::MsgConnectAck) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            std::memcpy(&ack, pkt.data(), sizeof(ack));
    const fl::EntityId ac{ack.assignedEntityIdx, ack.assignedEntityGen};
    REQUIRE(ac.valid());
    wb.onTick(1.0 / 60.0, 1u);
    REQUIRE(em.get(ac) != nullptr);

    wb.onDisconnect(1u);
    wb.onTick(1.0 / 60.0, 2u);
    CHECK(em.get(ac) == nullptr); // single-occupant aircraft despawns exactly as before
}

TEST_CASE("WorldBroadcaster: seats/set_seat operator surface reads and forces occupancy (#974)",
          "[world_broadcaster][crew]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());
    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr;
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);

    fl::EntityTransform t{};
    t.pos[1] = 800.0;
    t.quat[3] = 1.f;
    const fl::EntityId bomber = em.spawn("test:crewbomber", t);
    wb.registerController(bomber, std::make_unique<StillCtl>(), nullptr, 0.f);

    // crewRosterText lists both seats; the gunner defaults to bot occupancy.
    const std::string text = wb.crewRosterText(bomber.index);
    CHECK(text.find("seat 0") != std::string::npos);
    CHECK(text.find("[fly]") != std::string::npos);
    CHECK(text.find("seat 1") != std::string::npos);

    // A single-seat entity has no crew roster.
    connectPilotPeer(wb, net, 5u); // spawns a single-seat debug aircraft
    fl::MsgConnectAck ack{};
    for (const auto& pkt : net.sends)
        if (pkt.size() >= sizeof(fl::MsgConnectAck) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            std::memcpy(&ack, pkt.data(), sizeof(ack));
    CHECK(wb.crewRosterText(ack.assignedEntityIdx).find("single-seat") != std::string::npos);

    // set_seat: force the gunner seat empty, then back to bot.
    CHECK(wb.adminSetSeat(bomber.index, 1, fl::SeatOccupancy::Empty, 0).empty());
    CHECK(wb.crewRosterText(bomber.index).find("seat 1 (tail-gunner): empty") != std::string::npos);
    CHECK(wb.adminSetSeat(bomber.index, 1, fl::SeatOccupancy::Bot, 0).empty());
    CHECK(wb.crewRosterText(bomber.index).find("seat 1 (tail-gunner): bot") != std::string::npos);

    // The Fly seat is not settable via set_seat.
    CHECK_FALSE(wb.adminSetSeat(bomber.index, 0, fl::SeatOccupancy::Empty, 0).empty());
    // An out-of-range seat is rejected.
    CHECK_FALSE(wb.adminSetSeat(bomber.index, 9, fl::SeatOccupancy::Bot, 0).empty());
}

TEST_CASE("WorldBroadcaster: two humans on one crewed airframe each get an own record (#972/#980)",
          "[world_broadcaster][crew]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());
    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr;
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);

    // Peer 1 owns a crewed bomber; peer 2 joins its gunner seat. Both now occupy a seat in the SAME
    // airframe, so each must receive that airframe as its OWN record (omega + loadout block) — the
    // "isOwn generalizes to occupies-a-seat-here" property.
    connectPilotPeer(wb, net, 1u, "test:crewbomber");
    fl::MsgConnectAck ack{};
    for (const auto& pkt : net.sends)
        if (pkt.size() >= sizeof(fl::MsgConnectAck) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            std::memcpy(&ack, pkt.data(), sizeof(ack));
    const fl::EntityId bomber{ack.assignedEntityIdx, ack.assignedEntityGen};
    REQUIRE(bomber.valid());

    connectPilotPeer(wb, net, 2u);
    const fl::MsgSeatRequest r = joinReq(bomber, 1);
    wb.onReceive(2u, &r, sizeof(r));
    REQUIRE(wb.occupantPeerFor(bomber, 1) == 2u);

    clearSnapshots(net);
    wb.onTick(1.0 / 60.0, 1u);

    auto ownRecordFor = [&](uint32_t peerId) -> bool {
        for (const auto& pkt : snapshotsFor(net, peerId))
            for (const auto& e : decodeEntities(pkt))
                if (e.entityIdx == bomber.index && e.hasLoadout)
                    return true; // an own record (omega/loadout block present)
        return false;
    };
    CHECK(ownRecordFor(1u)); // the pilot gets the bomber as its own record
    CHECK(ownRecordFor(2u)); // the gunner ALSO gets the bomber as its own record
}

// ---------------------------------------------------------------------------
// Articulation on the wire (#843)
//
// Before this, no articulation channel reached a remote client, so even once the renderer could pose
// an aircraft every OTHER aircraft would sit with its gear up and its flaps clean. And a human pilot
// could not raise the gear at all: only Lua AI ever set gear_down, server-side.
// ---------------------------------------------------------------------------

namespace {} // namespace

TEST_CASE("WorldBroadcaster: MsgClientInput articulation commands reach the flight sim (#843)",
          "[world_broadcaster][articulation]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);
    // The flood-rate guard counts inputs per WALL second; a test loop sends hundreds instantly, so
    // raise the multiplier rather than have the guard silently drop the inputs under test.
    wb.setRateLimitParams(100, 10, 1000);
    connectPilotPeer(wb, net, 0u);

    // Gear down + half flaps, held long enough to travel.
    for (uint32_t t = 0; t < 400; ++t) {
        sendArticulationInput(wb, 0u, t + 1u, fl::kArtButtonGearDown, 128u);
        wb.onTick(1.0 / 60.0, t + 1u);
    }

    uint32_t pilotIdx = UINT32_MAX;
    em.forEach([&](const fl::EntityState& st) {
        if (pilotIdx == UINT32_MAX)
            pilotIdx = st.id.index;
    });
    REQUIRE(pilotIdx != UINT32_MAX);
    const auto* sim = wb.integratorFor(pilotIdx);
    REQUIRE(sim != nullptr);
    CHECK(sim->state().articulation.gear == Catch::Approx(1.0).margin(1e-3));
    CHECK(sim->state().articulation.flaps == Catch::Approx(128.f / 255.f).margin(0.01));

    // ...and back up. Absolute state, so simply clearing the bit retracts it.
    for (uint32_t t = 400; t < 800; ++t) {
        sendArticulationInput(wb, 0u, t + 1u, 0u, 0u);
        wb.onTick(1.0 / 60.0, t + 1u);
    }
    CHECK(sim->state().articulation.gear == Catch::Approx(0.0).margin(1e-3));
    CHECK(sim->state().articulation.flaps == Catch::Approx(0.0).margin(1e-3));
}

// ── replay tap (#643) ───────────────────────────────────────────────────────
//
// The recorder cannot reuse a per-peer snapshot: those are interest-filtered and budget-capped, so
// they describe what one player could see rather than what happened. These cases pin the two
// properties that makes the tap worth having -- it sees EVERYTHING, and it costs nothing when
// nobody is recording.

// ── sim determinism (#644) ──────────────────────────────────────────────────
//
// #644 asks for two properties that a single gate cannot honestly cover. The record<->replay
// FIDELITY half lives in `replay_roundtrip_ci_smoke` (it needs a real recording on disk). This is the
// other half, and the one the roadmap actually calls the sim-drift alarm: does the SIM produce the
// same world twice?
//
// It runs IN PROCESS on purpose. Two networked server runs are not tick-aligned -- input arrival
// decides which tick an input lands on -- so a two-process comparison would be a flaky test wearing a
// determinism gate's clothes. Stepping the same scripted world twice, and at 1 vs N workers, is
// deterministic by construction, and it is what actually fails when someone introduces a race, an
// unseeded RNG, or a float path that depends on evaluation order.

namespace {

// A controller with a fixed, per-instance control input -- enough for every entity to fly its own
// trajectory without pulling engine-ai into this test.
struct ScriptedController : fl::IEntityController {
    fl::ControlInput ctrl{};
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext&) override {
        return ctrl;
    }
};

// Step a fixed scripted world and return the per-tick state-hash stream from the replay tap.
std::vector<uint64_t> runDeterminismScenario(fl::JobSystem* jobs, int entityCount = 24) {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    std::vector<uint64_t> hashes;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.snapshot.replaySink = [&hashes](const fl::ReplayTickRecords& r) { hashes.push_back(r.stateHash); };
    h_broadcaster.snapshot.replayKeyframeIntervalTicks = 30;
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));
    if (jobs)
        broadcaster.setJobSystem(*jobs);
    connectPilotPeer(broadcaster, net, 0u);

    // A moving world: entities under AI control, spread out so the spatial index, the AI pass and the
    // integrate pass all have real work to parallelise. A static world would pass this gate while
    // hiding every ordering bug it exists to catch.
    for (int i = 0; i < entityCount; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 500.0 * static_cast<double>(i);
        t.pos[1] = 1200.0 + 5.0 * static_cast<double>(i);
        t.pos[2] = -300.0 * static_cast<double>(i % 7);
        const fl::EntityId id = em.spawn("builtin:debug-entity", t);
        REQUIRE(id.valid());
        // Per-entity control surfaces, so the entities fly DIFFERENT paths: a fleet flying one
        // identical trajectory would hash the same under any ordering bug.
        auto ctl = std::make_unique<ScriptedController>();
        ctl->ctrl.throttle = 0.5f + 0.02f * static_cast<float>(i % 10);
        ctl->ctrl.elevator = 0.1f * static_cast<float>((i % 5) - 2);
        ctl->ctrl.aileron = 0.05f * static_cast<float>((i % 3) - 1);
        broadcaster.registerController(id, std::move(ctl));
    }

    for (uint64_t tick = 1; tick <= 180; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);
    return hashes;
}

} // namespace

TEST_CASE("WorldBroadcaster: the same scripted world hashes identically twice (#644)",
          "[world_broadcaster][determinism]") {
    const std::vector<uint64_t> first = runDeterminismScenario(nullptr);
    const std::vector<uint64_t> second = runDeterminismScenario(nullptr);

    REQUIRE(first.size() == 180);
    CHECK(first == second);

    // The stream must actually be doing something: an all-identical stream would compare equal while
    // proving nothing, which is exactly how a determinism gate quietly stops testing anything.
    CHECK(std::adjacent_find(first.begin(), first.end(), std::not_equal_to<>()) != first.end());
}

TEST_CASE("WorldBroadcaster: worker count does not change the world (#644)", "[world_broadcaster][determinism]") {
    const std::vector<uint64_t> serial = runDeterminismScenario(nullptr);

    for (unsigned workers : {1u, 2u, 4u, 8u}) {
        fl::JobSystem jobs(workers);
        const std::vector<uint64_t> parallel = runDeterminismScenario(&jobs);
        INFO("workers=" << workers);
        REQUIRE(parallel.size() == serial.size());
        // Tick-by-tick rather than whole-vector, so a failure names WHEN the sim diverged -- which is
        // the first thing anyone chasing a drift regression needs.
        for (std::size_t i = 0; i < serial.size(); ++i) {
            INFO("tick " << (i + 1));
            REQUIRE(parallel[i] == serial[i]);
        }
    }
}

TEST_CASE("WorldBroadcaster: the state hash notices a changed world (#644)", "[world_broadcaster][determinism]") {
    // The gate is only worth having if the hash actually responds to state. One more entity must
    // produce a different stream -- otherwise "identical hashes" would mean nothing at all.
    const std::vector<uint64_t> base = runDeterminismScenario(nullptr, 24);
    const std::vector<uint64_t> more = runDeterminismScenario(nullptr, 25);
    REQUIRE(base.size() == more.size());
    CHECK(base != more);
}

// ── #576: per-peer throttle attribution + the server-throttle TLV ────────────────────────────────

namespace {} // namespace

// ---------------------------------------------------------------------------
// #1069: rate limits on the world-mutating request channels, and the handshake gate
// ---------------------------------------------------------------------------
// The defect these cover: three client->server messages answered back with no limit at all. The
// assertion that matters most is the AMPLIFICATION BOUND — bytes out per byte in — because the ratio
// is the actual defect, not the request count.

// Total bytes this peer was sent, across unicast and broadcast.
static std::size_t bytesSentTo(const MockNetwork& net, uint32_t peerId) {
    std::size_t total = 0;
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == peerId)
            total += pkt.size();
    for (const auto& b : net.broadcasts)
        total += b.size();
    return total;
}

static int countMsgsTo(const MockNetwork& net, uint32_t peerId, fl::MsgId id) {
    int n = 0;
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == peerId && !pkt.empty() && pkt[0] == static_cast<uint8_t>(id))
            ++n;
    return n;
}

TEST_CASE("WorldBroadcaster: heartbeat replies are rate-limited but liveness still counts (#1069)",
          "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::ManualClock clock;
    broadcaster.setClock(clock);
    broadcaster.setHeartbeatRateLimit(4);

    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1);
    net.sends.clear();
    net.perPeerSends.clear();

    fl::MsgHeartbeat hb{};
    hb.tickIndex = 1;
    for (int i = 0; i < 20; ++i)
        broadcaster.onReceive(0u, &hb, sizeof(hb));

    // 20 heartbeats in one window, 4 replies. Before the limit this was 20 — a 1:1 reflector.
    CHECK(countMsgsTo(net, 0u, fl::MsgId::PeerDelay) == 4);

    // The window rolls: the next second gets its own budget.
    clock.advance(std::chrono::seconds(2));
    net.perPeerSends.clear();
    for (int i = 0; i < 20; ++i)
        broadcaster.onReceive(0u, &hb, sizeof(hb));
    CHECK(countMsgsTo(net, 0u, fl::MsgId::PeerDelay) == 4);

    // Liveness is still accounted for every packet, INCLUDING the unanswered ones: a peer sending far
    // over the reply budget must not idle-time itself out in a way a well-behaved peer cannot. Ten
    // heartbeats every half-second, against a 1 s idle timeout, for five seconds of sim time.
    broadcaster.setIdleTimeout(1);
    for (uint64_t tick = 2; tick <= 300; ++tick) {
        broadcaster.onTick(1.0 / 60.0, tick);
        if (tick % 30 == 0) {
            clock.advance(std::chrono::milliseconds(500));
            hb.tickIndex = tick;
            for (int i = 0; i < 10; ++i)
                broadcaster.onReceive(0u, &hb, sizeof(hb));
        }
    }
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: seat requests are rate-limited and over-limit ones are silent (#1069)",
          "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());
    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr;
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    fl::ManualClock clock;
    wb.setClock(clock);
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);
    wb.setSeatRequestRateLimit(2);

    fl::EntityTransform t{};
    t.pos[1] = 800.0;
    t.quat[3] = 1.f;
    const fl::EntityId bomber = em.spawn("test:crewbomber", t);
    wb.registerController(bomber, std::make_unique<StillCtl>(), nullptr, 0.f);

    connectPilotPeer(wb, net, 1u);
    net.sends.clear();
    net.perPeerSends.clear();

    // Ten requests for a seat that does not exist: all cheap denials, but each one used to draw a
    // reliable MsgSeatResult. Only the first two are answered now.
    const fl::MsgSeatRequest bad = joinReq(bomber, 99);
    for (int i = 0; i < 10; ++i)
        wb.onReceive(1u, &bad, sizeof(bad));
    CHECK(countMsgsTo(net, 1u, fl::MsgId::SeatResult) == 2);

    clock.advance(std::chrono::seconds(2));
    net.perPeerSends.clear();
    for (int i = 0; i < 10; ++i)
        wb.onReceive(1u, &bad, sizeof(bad));
    CHECK(countMsgsTo(net, 1u, fl::MsgId::SeatResult) == 2);
}

TEST_CASE("WorldBroadcaster: a seat-request flood cannot amplify past the limiter (#1069)",
          "[world_broadcaster][security]") {
    // The amplification RATIO is the defect: a 12-byte MsgSeatRequest whose grant re-sends the whole
    // ConnectAck type table was ~1900x at a realistic registry. This bounds bytes-out/bytes-in for a
    // flood, which is what an attacker actually controls.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());
    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr;
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    fl::ManualClock clock;
    wb.setClock(clock);
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);
    wb.setSeatRequestRateLimit(2);

    fl::EntityTransform t{};
    t.pos[1] = 800.0;
    t.quat[3] = 1.f;
    const fl::EntityId bomber = em.spawn("test:crewbomber", t);
    wb.registerController(bomber, std::make_unique<StillCtl>(), nullptr, 0.f);

    connectPilotPeer(wb, net, 1u);
    net.sends.clear();
    net.perPeerSends.clear();
    net.broadcasts.clear();

    // A hop between the gunner seat and back, flooded: every accepted grant is a despawn/respawn plus
    // a full ConnectAck, so an unlimited flood is unbounded output for 12 bytes in per packet.
    const fl::MsgSeatRequest join = joinReq(bomber, 1);
    fl::MsgSeatRequest leave{};
    leave.msgId = static_cast<uint8_t>(fl::MsgId::SeatRequest);
    leave.flags = fl::kSeatRequestFlagLeave;

    std::size_t bytesIn = 0;
    for (int i = 0; i < 50; ++i) {
        const fl::MsgSeatRequest& req = (i % 2 == 0) ? join : leave;
        wb.onReceive(1u, &req, sizeof(req));
        bytesIn += sizeof(req);
    }
    const std::size_t bytesOut = bytesSentTo(net, 1u);

    // With the limiter only 2 of the 50 are served in this window, and since #1070 a re-ack no longer
    // drags the entity-type table along, so the served ones are small too. Both halves of the fix show
    // up here: without the limiter the output grows linearly with the flood, and without the type-table
    // skip each served request alone would blow this bound at a realistic registry.
    CHECK(bytesIn == 50u * sizeof(fl::MsgSeatRequest));
    CHECK(bytesOut < 4u * bytesIn);
}

TEST_CASE("WorldBroadcaster: team switches are on a cooldown, and a rejected one is silent (#1069)",
          "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    int guardCalls = 0;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.match.teamSwitchGuard = [&guardCalls](uint32_t, uint16_t) {
        ++guardCalls;
        return true;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));
    fl::ManualClock clock;
    broadcaster.setClock(clock);
    broadcaster.setTeamSwitchCooldownSeconds(5);

    connectPilotPeer(broadcaster, net, 0u);
    net.sends.clear();
    net.perPeerSends.clear();

    fl::MsgTeamRequest req{};
    req.msgId = static_cast<uint8_t>(fl::MsgId::TeamRequest);
    req.factionIndex = 1;
    for (int i = 0; i < 20; ++i)
        broadcaster.onReceive(0u, &req, sizeof(req));

    // One accepted switch in the cooldown window; the other 19 never reach the guard at all.
    CHECK(guardCalls == 1);

    // The cooldown starts at the ACCEPTED request, so spamming does not extend it.
    clock.advance(std::chrono::seconds(6));
    broadcaster.onReceive(0u, &req, sizeof(req));
    CHECK(guardCalls == 2);

    // A cooled-down rejection sends nothing — a notice per rejected packet would be the amplifier.
    net.perPeerSends.clear();
    for (int i = 0; i < 10; ++i)
        broadcaster.onReceive(0u, &req, sizeof(req));
    CHECK(guardCalls == 2);
    CHECK(countMsgsTo(net, 0u, fl::MsgId::ServerNotice) == 0);
}

TEST_CASE("WorldBroadcaster: an unadmitted peer's client input is ignored (#1069)", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    // onConnect ONLY — no MsgConnectRequest, so the peer has an input slot but is not admitted (#853).
    broadcaster.onConnect(0u);
    net.sends.clear();
    net.perPeerSends.clear();

    // Before #1069 the MsgClientInput branch had no handshake gate while chat/voice/seat/team all did,
    // so an unadmitted peer could drive the jitter buffer, the EWMA estimators and the trace writer.
    // Advance the server's tick first so an ACCEPTED input (tickIndex 0) would leave a large,
    // visible estimatedDelayTicks behind — the state this asserts stays clean.
    for (uint64_t tick = 1; tick <= 100; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);
    const auto pkt = makeClientInputPacket();
    for (int i = 0; i < 10; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());

    // A heartbeat from the same unadmitted peer is dropped too — no reply to an unauthenticated peer.
    fl::MsgHeartbeat hb{};
    broadcaster.onReceive(0u, &hb, sizeof(hb));
    CHECK(countMsgsTo(net, 0u, fl::MsgId::PeerDelay) == 0);

    // Admit the peer now. Admission REUSES the PeerInputState created at onConnect, so anything the
    // pre-handshake flood had managed to write would still be sitting there — which is exactly how a
    // poisoned delay estimate would have survived into the admitted session.
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    broadcaster.onReceive(0u, &req, sizeof(req));

    bool found = false;
    broadcaster.forEachPeer([&](const fl::PeerInfo& p) {
        if (p.peerId == 0u) {
            found = true;
            CHECK(p.delayTicks == 0u); // no delay estimate was taken from the unadmitted packets
            CHECK(p.queueDepth == 0u); // and none of them reached the jitter buffer
        }
    });
    CHECK(found);
}

// ---------------------------------------------------------------------------
// #1070: the connect-ack entity-type table is sent once, not on every re-ack
// ---------------------------------------------------------------------------

TEST_CASE("EntityTypeRegistry: generation distinguishes a cleared table from an unchanged one (#1070)",
          "[world_broadcaster]") {
    // typeCount() cannot answer "has this table changed" — clear() then re-register the same number of
    // types leaves the count identical. The connect-ack skip keys on generation() for exactly this.
    fl::EntityTypeRegistry registry;
    CHECK(registry.generation() == 0u);

    registry.registerType(makeDebugDef());
    const uint32_t afterOne = registry.generation();
    CHECK(afterOne != 0u);

    // A duplicate id registers nothing, so it must not move the generation.
    registry.registerType(makeDebugDef());
    CHECK(registry.generation() == afterOne);

    const uint32_t countBefore = registry.typeCount();
    registry.clear();
    registry.registerType(makeDebugDef());
    CHECK(registry.typeCount() == countBefore); // same count...
    CHECK(registry.generation() != afterOne);   // ...different table, and the generation says so
}

// ---------------------------------------------------------------------------
// #1091: the scoreboard is built once per window, not once per peer
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// #1090: the voice relay fan-out is bounded without touching a realistic session
// ---------------------------------------------------------------------------

namespace {} // namespace

TEST_CASE("WorldBroadcaster: an all-mics-open session cannot fan out without bound (#1090)",
          "[world_broadcaster][voice]") {
    // The abuse case the issue exists for: EVERY peer transmitting continuously. Without the talker
    // cap this is (talkers x listeners) — at 128 players roughly 975,000 sendChannel calls a second.
    // With it, only maxTalkers peers per net relay at any moment, so the fan-out is bounded by
    // (cap x listeners) no matter how many mics are open.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::ManualClock clock;
    broadcaster.setClock(clock);
    broadcaster.setVoiceEnabled(true);

    constexpr int kPeers = 16;
    for (uint32_t p = 0; p < kPeers; ++p)
        connectPilotPeer(broadcaster, net, p);
    broadcaster.onTick(1.0 / 60.0, 1);

    const fl::RadioNetDef* def = broadcaster.radioNets().byIndex(0);
    REQUIRE(def != nullptr);
    const uint32_t cap = def->maxTalkers;
    REQUIRE(cap > 0u);

    const uint64_t before = broadcaster.voiceRelaySendCount();
    // One frame from every peer, all in the same instant — 16 open mics.
    for (uint32_t p = 0; p < kPeers; ++p) {
        const auto frame = makeVoiceFrame(0, 1);
        broadcaster.onReceive(p, frame.data(), frame.size());
    }
    const uint64_t sends = broadcaster.voiceRelaySendCount() - before;

    // At most cap talkers relayed, each to at most every other peer. Without the cap this bound is
    // kPeers x (kPeers - 1) = 240; with it, 4 x 15 = 60.
    CHECK(sends <= static_cast<uint64_t>(cap) * static_cast<uint64_t>(kPeers - 1));
    CHECK(sends < static_cast<uint64_t>(kPeers) * static_cast<uint64_t>(kPeers - 1));
}
