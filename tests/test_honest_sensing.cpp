// SPDX-License-Identifier: GPL-3.0-or-later
//
// #670 acceptance: honest AI sensing, end-to-end through a real WorldBroadcaster tick.
//
// The sub-tasks each have unit tests for their own piece. These are the cross-cutting scenarios that
// no single piece can prove on its own — the ones that answer "is the AI actually honest now?"
// rather than "does this function return the right number?".

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/StateMachineController.h"
#include "config/DifficultySettings.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "job/JobSystem.h"
#include "net/WorldBroadcaster.h"
#include "sensor/SensorSystem.h"
#include "weather/WeatherController.h"

#include "mock_hal.h"
#include "mock_network.h"

#include <memory>
#include <string>
#include <vector>

using namespace fl;

namespace {

constexpr float kMPerNm = 1852.f;
constexpr double kDt = 1.0 / 60.0;

constexpr uint16_t kBlue = 1;
constexpr uint16_t kRed = 2;

// A forward-looking radar: ±60° search out to 40 nm. Deliberately blind astern.
std::shared_ptr<const sensor::SensorDef> makeRadar() {
    sensor::SensorDef s;
    s.id = "t:radar";
    s.type = sensor::SensorType::Radar;
    s.emitter = true;
    s.search = sensor::SensorLobe{60.f, 30.f, 0.f, 40.f * kMPerNm, 1.f}; // pod 1: geometry under test
    s.track = sensor::SensorLobe{30.f, 20.f, 0.f, 30.f * kMPerNm, 1.f};
    s.lockHoldS = 4.f;
    return std::make_shared<const sensor::SensorDef>(s);
}

// A visual-only unit: no radar, and therefore something the DARK can take away.
std::shared_ptr<const sensor::SensorDef> makeEye() {
    sensor::SensorDef s;
    s.id = "t:eye";
    s.type = sensor::SensorType::Visual;
    s.search = sensor::SensorLobe{90.f, 60.f, 0.f, 10.f * kMPerNm, 0.30f};
    return std::make_shared<const sensor::SensorDef>(s);
}

EntityDef makeDef(const char* id, std::vector<std::string> sensors) {
    EntityDef d;
    d.id = id;
    d.name = "T";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    d.sensorIds = std::move(sensors);
    return d;
}

// A WorldBroadcaster wired the way fl-server wires it: a sensor-def resolver, a registry of types
// that carry sensors, and (optionally) weather.
struct SensingFixture {
    MockLogger logger;
    TrackingNetwork net;
    EntityTypeRegistry registry;
    EntityManager em{logger, registry};
    WeatherController weather;
    std::unique_ptr<WorldBroadcaster> wb;

    explicit SensingFixture(bool withWeather = false) {
        registry.registerType(makeDef("t:radar-unit", {"t:radar"}));
        registry.registerType(makeDef("t:eye-unit", {"t:eye"}));
        registry.registerType(makeDef("t:target", {})); // a target that carries nothing

        fl::WorldQueries q_wb;
        q_wb.sensorDefs = [](const std::string& id) -> std::shared_ptr<const sensor::SensorDef> {
            if (id == "t:radar")
                return makeRadar();
            if (id == "t:eye")
                return makeEye();
            return nullptr;
        };
        wb = std::make_unique<WorldBroadcaster>(em, registry, net, logger, withWeather ? &weather : nullptr,
                                                std::move(q_wb));
        wb->setSensorCheckHz(60.f); // a check every tick: these tests are about WHAT is seen, not when
    }

    EntityId spawn(const char* type, double x, double y, double z, uint16_t faction) {
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        t.quat[3] = 1.f; // identity: nose along +X
        const EntityId id = em.spawn(type, t);
        if (EntityState* st = em.get(id))
            st->factionIndex = faction;
        return id;
    }

    // Register a controller so the entity becomes an OBSERVER (addControlledEntity is what creates
    // one), exactly as a spawned AI does on a real server.
    void makeObserver(EntityId id) {
        wb->registerController(id, std::make_unique<ai::StateMachineController>(em));
    }

    void tick(uint64_t n) {
        for (uint64_t i = 1; i <= n; ++i)
            wb->onTick(kDt, i);
    }

    const sensor::ContactTable* contacts(EntityId id) const {
        return wb->contactsFor(id.index);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// #670 acceptance 1: "AI does not react to a target outside its sensor cones"
// ---------------------------------------------------------------------------

TEST_CASE("Honest sensing: a bandit behind a forward-looking AI is invisible to it", "[honest_sensing]") {
    SensingFixture f;

    const EntityId observer = f.spawn("t:radar-unit", 0, 5000, 0, kBlue);
    f.makeObserver(observer);

    const EntityId ahead = f.spawn("t:target", 10.0 * kMPerNm, 5000, 0, kRed);   // in the cone
    const EntityId astern = f.spawn("t:target", -10.0 * kMPerNm, 5000, 0, kRed); // behind: outside it

    f.tick(2);

    const sensor::ContactTable* c = f.contacts(observer);
    REQUIRE(c != nullptr);

    // Both bandits are alive, hostile, and the same distance away. One of them does not exist as far
    // as this AI is concerned — and that is the entire point of #670.
    CHECK(c->find(ahead) != nullptr);
    CHECK(c->find(astern) == nullptr);
}

// ---------------------------------------------------------------------------
// #670 acceptance 2: reaction delay is real and is scaled by difficulty
// ---------------------------------------------------------------------------

TEST_CASE("Honest sensing: a contact is DETECTED before it is REACTED to", "[honest_sensing]") {
    SensingFixture f;

    AiScaling scaling{};
    scaling.radarSensorRange = 1.f;
    scaling.reactionTimeS = 1.f; // 1 s at 60 Hz = 60 ticks
    f.wb->setAiScaling(scaling);

    const EntityId observer = f.spawn("t:radar-unit", 0, 5000, 0, kBlue);
    f.makeObserver(observer);
    const EntityId bandit = f.spawn("t:target", 10.0 * kMPerNm, 5000, 0, kRed);

    f.tick(1);
    const sensor::Contact* c = f.contacts(observer)->find(bandit);
    REQUIRE(c != nullptr);
    CHECK(c->held());        // it sees him immediately...
    CHECK_FALSE(c->reacted); // ...and has not yet noticed him

    // Seeing is not noticing. An AI that acted the instant it detected would have superhuman
    // reflexes on every difficulty setting.
    f.tick(70);
    CHECK(f.contacts(observer)->find(bandit)->reacted);
}

TEST_CASE("Honest sensing: Cadet radar reaches half as far as Ace radar", "[honest_sensing]") {
    // The difficulty knob gates ACTING and (for radar) RANGE — never the ability to see at all.
    // A target between the two ranges is the whole test: the Ace finds him, the Cadet does not.
    const double between = 30.0 * kMPerNm; // inside 40 nm (Ace ×1.0), outside 20 nm (Cadet ×0.5)

    auto firstDetection = [between](float radarFraction) {
        SensingFixture f;
        AiScaling scaling{};
        scaling.radarSensorRange = radarFraction;
        scaling.reactionTimeS = 0.f;
        f.wb->setAiScaling(scaling);

        const EntityId observer = f.spawn("t:radar-unit", 0, 5000, 0, kBlue);
        f.makeObserver(observer);
        const EntityId bandit = f.spawn("t:target", between, 5000, 0, kRed);

        f.tick(4);
        return f.contacts(observer)->find(bandit) != nullptr;
    };

    CHECK(firstDetection(1.0f) == true);  // Ace: full authored range
    CHECK(firstDetection(0.5f) == false); // Cadet: half of it, and the bandit is beyond
}

// ---------------------------------------------------------------------------
// #670 acceptance 3: weather and darkness (via #209)
// ---------------------------------------------------------------------------

TEST_CASE("Honest sensing: a visual-only unit acquires more slowly at night than at noon", "[honest_sensing]") {
    // Identical geometry, identical seeds, different clock. The night unit needs more checks to find
    // the same target — which is why night attacks work.
    auto ticksToAcquire = [](float timeOfDay) -> int {
        SensingFixture f(/*withWeather=*/true);
        f.weather.setTimeOfDay(timeOfDay);

        const EntityId observer = f.spawn("t:eye-unit", 0, 5000, 0, kBlue);
        f.makeObserver(observer);
        const EntityId bandit = f.spawn("t:target", 3.0 * kMPerNm, 5000, 0, kRed);

        for (int t = 1; t <= 600; ++t) {
            f.wb->onTick(kDt, static_cast<uint64_t>(t));
            if (f.contacts(observer)->find(bandit))
                return t;
        }
        return 601; // never acquired within 10 s of checks
    };

    const int noon = ticksToAcquire(12.f);
    const int night = ticksToAcquire(2.f);

    CHECK(noon <= night); // the dark never HELPS an eyeball
    CHECK(noon < 601);    // and in daylight it does find him
}

// ---------------------------------------------------------------------------
// #670 acceptance 4: determinism — the property replay and the scale gate rest on
// ---------------------------------------------------------------------------

TEST_CASE("Honest sensing: contact tables are identical across JobSystem worker counts", "[honest_sensing]") {
    // The sensing pass runs data-parallel. If the contact tables depended on how many cores the
    // server happened to have, the same server would make different decisions on different hardware
    // — a desync generator, and the end of any hope of replay (#644).
    auto runWith = [](unsigned workers) {
        SensingFixture f;
        JobSystem jobs(workers);
        f.wb->setJobSystem(jobs);

        std::vector<EntityId> observers;
        for (int i = 0; i < 8; ++i) {
            const EntityId o = f.spawn("t:radar-unit", i * 500.0, 5000, 0, (i % 2) ? kBlue : kRed);
            f.makeObserver(o);
            observers.push_back(o);
        }
        for (int i = 0; i < 8; ++i)
            f.spawn("t:target", 4000.0 + i * 900.0, 5000, i * 300.0, (i % 2) ? kRed : kBlue);

        f.tick(10);

        std::string flat;
        for (const EntityId& o : observers) {
            const sensor::ContactTable* tbl = f.contacts(o);
            flat += std::to_string(o.index) + ":";
            if (tbl) {
                for (const sensor::Contact& c : *tbl) {
                    flat += std::to_string(c.id.index) + "/" + std::to_string(static_cast<int>(c.state)) + "/" +
                            std::to_string(c.lastSeenTick) + "/" + std::to_string(c.reacted ? 1 : 0) + ",";
                }
            }
            flat += ";";
        }
        return flat;
    };

    const std::string serial = runWith(1);
    const std::string parallel = runWith(4);

    CHECK_FALSE(serial.empty());
    CHECK(serial == parallel); // byte-identical on 1 worker and on 4
}

// ---------------------------------------------------------------------------
// The whole point, in one test
// ---------------------------------------------------------------------------

TEST_CASE("Honest sensing: ground truth is not reachable from a contact table", "[honest_sensing]") {
    // A contact reports where the target WAS when it was last seen. Move the target while it is
    // outside the cone and the AI keeps believing the old position — it is steering at a memory, and
    // there is no path from here to the truth.
    SensingFixture f;

    const EntityId observer = f.spawn("t:radar-unit", 0, 5000, 0, kBlue);
    f.makeObserver(observer);
    const EntityId bandit = f.spawn("t:target", 10.0 * kMPerNm, 5000, 0, kRed);

    f.tick(2);
    const sensor::Contact* seen = f.contacts(observer)->find(bandit);
    REQUIRE(seen != nullptr);
    const double knownX = seen->lastKnownPos[0];

    // He breaks astern — out of the cone entirely — and keeps running.
    EntityState* st = f.em.get(bandit);
    REQUIRE(st != nullptr);
    st->transform.pos[0] = -20.0 * kMPerNm;

    f.tick(3);
    const sensor::Contact* coasting = f.contacts(observer)->find(bandit);
    REQUIRE(coasting != nullptr);
    CHECK(coasting->state == sensor::ContactState::Coasting);

    // The AI's picture of the world is now WRONG, and honestly so: it still has him out in front.
    CHECK(coasting->lastKnownPos[0] == Catch::Approx(knownX));
    CHECK(st->transform.pos[0] < 0.0); // where he really is
}
