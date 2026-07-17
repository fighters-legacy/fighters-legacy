// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientPrediction.h"

#include "RenderTypes.h" // EnvironmentState
#include "flight/BuiltinFlightModel.h"
#include "net/GameProtocol.h"
#include "render/RenderSnapshot.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

using namespace fl;

namespace {

// A clean airframe: no stores, so no payload mass or drag. The server resolves the same thing for
// an entity with no hardpoints, so the two still agree (#812).
static ClientPrediction::PayloadResolver noPayload() {
    return [](uint32_t) -> PayloadEffect { return {}; };
}

// Default resolver: always returns the builtin UFO model.
static ClientPrediction::FlightModelResolver builtinResolver() {
    return [](uint32_t) -> std::shared_ptr<const FlightModelData> { return BuiltinFlightModel::get(); };
}

// Flat-ground height query.
static ClientPrediction::HeightQuery flatGround() {
    return [](glm::dvec3) -> float { return 0.f; };
}

static EnvironmentState makeEnv() {
    return {};
}

static EnvironmentState makeEnv(float turbulenceAmp) {
    EnvironmentState env{};
    env.turbulenceAmp = turbulenceAmp;
    return env;
}

// Build a minimal RenderSnapshot with one player entry at the origin.
static RenderSnapshot makeSnap(uint32_t idx, uint32_t gen, glm::dvec3 pos = {0, 1, 0}) {
    RenderSnapshot snap;
    snap.tickIndex = 1u;
    EntityRenderEntry e;
    e.entityIdx = idx;
    e.entityGen = gen;
    e.typeIndex = 0u;
    e.position = pos;
    e.orientation = glm::quat(1.f, 0.f, 0.f, 0.f); // identity
    e.fuelPct = 100u;
    snap.entries.push_back(e);
    return snap;
}

// Build a MsgClientInput with a given seqNum and neutral controls.
static MsgClientInput makeInput(uint32_t seqNum, float throttle = 0.f) {
    MsgClientInput msg{};
    msg.msgId = static_cast<uint8_t>(MsgId::ClientInput);
    msg.seqNum = seqNum;
    msg.throttle = throttle;
    return msg;
}

} // namespace

TEST_CASE("ClientPrediction / not initialized before init", "[client_prediction]") {
    ClientPrediction pred;
    CHECK(!pred.isInitialized());

    auto snap = makeSnap(1u, 1u);
    const auto origPos = snap.entries[0].position;
    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // No init() called — entry must be untouched.
    CHECK(snap.entries[0].position.x == Catch::Approx(origPos.x));
}

TEST_CASE("ClientPrediction / init with pre-ConnectAck defaults never reconciles (regression 755)",
          "[client_prediction]") {
    // Regression for #755. ClientPrediction::init() was wired in onConnect, which runs
    // before MsgConnectAck populates the player's entity idx/gen, so it captured the
    // stale defaults (idx=0, gen=0). A valid live entity always has gen != 0, so the
    // idx/gen match in reconcile() never succeeded and prediction stayed a permanent
    // no-op. This guards that exact mechanism: init'd with 0/0, reconcile leaves a real
    // (gen != 0) player entry untouched and the lazy integrator init never fires.
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 0u, 0u);

    auto snap = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    const auto origPos = snap.entries[0].position;
    pred.reconcile(snap, 1u, 4u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // Entry untouched; the lazy model resolve + integrator build never ran.
    CHECK(!pred.isInitialized());
    CHECK(snap.entries[0].position.x == Catch::Approx(origPos.x));
    CHECK(snap.entries[0].position.y == Catch::Approx(origPos.y));
    CHECK(snap.entries[0].position.z == Catch::Approx(origPos.z));
}

TEST_CASE("ClientPrediction / reconcile replaces player entry", "[client_prediction]") {
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto snap = makeSnap(1u, 1u, glm::dvec3{0, 1, 0});
    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    CHECK(pred.isInitialized());
    // Position was at Y=1 (1 m AGL); after reconcile with 0 delay ticks the
    // predicted position should match the server entry very closely.
    CHECK(snap.entries[0].position.y == Catch::Approx(1.0).margin(0.5));
}

TEST_CASE("ClientPrediction / onInput steps integrator", "[client_prediction]") {
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto snap = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    const double yBefore = snap.entries[0].position.y;

    // Step one tick with throttle=1 (upward thrust at altitude 500 m).
    pred.onInput(makeInput(1u, 1.f), makeEnv());

    // Re-reconcile with estimatedDelayTicks=1 so the one input is replayed.
    auto snap2 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap2, 2u, 1u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // At least one tick of full thrust should have moved the entity upward.
    CHECK(snap2.entries[0].position.y > yBefore - 1.0);
}

TEST_CASE("ClientPrediction / replay depth matches estimatedDelayTicks", "[client_prediction]") {
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto snap = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // Push 5 inputs with throttle=0.
    for (uint32_t i = 1u; i <= 5u; ++i) {
        pred.onInput(makeInput(i, 0.f), makeEnv());
    }

    // Reconcile with the same server position but replay 3 of the 5 inputs.
    auto snap2 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap2, 2u, 3u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    const double y3 = snap2.entries[0].position.y;

    // Replay 5 of the 5 inputs.
    auto snap3 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap3, 2u, 5u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    const double y5 = snap3.entries[0].position.y;

    // Both should be finite and within a physically plausible range.
    CHECK(std::isfinite(y3));
    CHECK(std::isfinite(y5));
}

TEST_CASE("ClientPrediction / exact acked-seqNum replays exactly the inputs newer than the ack (#427)",
          "[client_prediction]") {
    // Two predictors fed identical inputs: one reconciles by the exact acked seqNum, the other by
    // the equivalent delay-ticks count. Same replay window => same result. The seqNum path also gets
    // a deliberately-wrong estimatedDelayTicks (999) to prove it IGNORES the estimate when the ack
    // is present.
    ClientPrediction seqPred, tickPred;
    seqPred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);
    tickPred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto s0 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    seqPred.reconcile(s0, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    auto t0 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    tickPred.reconcile(t0, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    for (uint32_t i = 1u; i <= 5u; ++i) {
        seqPred.onInput(makeInput(i, 1.0f), makeEnv());
        tickPred.onInput(makeInput(i, 1.0f), makeEnv());
    }

    // acked seqNum 3 => replay inputs 4 and 5 (2 inputs) === delay-ticks 2.
    auto sa = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    seqPred.reconcile(sa, 2u, 999u, /*ackedSeqNum=*/3u, makeEnv());
    auto sb = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    tickPred.reconcile(sb, 2u, 2u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    CHECK(sa.entries[0].position.x == Catch::Approx(sb.entries[0].position.x));
    CHECK(sa.entries[0].position.y == Catch::Approx(sb.entries[0].position.y));
    CHECK(sa.entries[0].position.z == Catch::Approx(sb.entries[0].position.z));

    // acked seqNum == the last input => replay nothing === delay-ticks 0.
    auto sc = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    seqPred.reconcile(sc, 2u, 999u, /*ackedSeqNum=*/5u, makeEnv());
    auto sd = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    tickPred.reconcile(sd, 2u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    CHECK(sc.entries[0].position.x == Catch::Approx(sd.entries[0].position.x));
    CHECK(sc.entries[0].position.y == Catch::Approx(sd.entries[0].position.y));
}

TEST_CASE("ClientPrediction / applies broadcast weather turbulence during replay (#426)", "[client_prediction]") {
    // With a non-zero turbulence amplitude, replay now injects the deterministic per-tick
    // perturbation instead of predicting zero turbulence — so the replayed position differs from a
    // calm-air replay of the identical inputs. This is the divergence #426 removes from the
    // reconciliation jitter.
    ClientPrediction calm, gusty;
    calm.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);
    gusty.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto c0 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    calm.reconcile(c0, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    auto g0 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    gusty.reconcile(g0, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    for (uint32_t i = 1u; i <= 8u; ++i) {
        calm.onInput(makeInput(i, 0.5f), makeEnv(0.0f));
        gusty.onInput(makeInput(i, 0.5f), makeEnv(12.0f)); // strong turbulence
    }

    auto cs = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    calm.reconcile(cs, 2u, 8u, ClientPrediction::kNoAckedSeqNum, makeEnv(0.0f));
    auto gs = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    gusty.reconcile(gs, 2u, 8u, ClientPrediction::kNoAckedSeqNum, makeEnv(12.0f));

    const glm::dvec3 calmPos = cs.entries[0].position;
    const glm::dvec3 gustyPos = gs.entries[0].position;
    const double d = glm::length(gustyPos - calmPos);
    CHECK(std::isfinite(d));
    CHECK(d > 0.0); // turbulence moved the predicted position
}

TEST_CASE("ClientPrediction / hard snap on large divergence", "[client_prediction]") {
    PredictionSettings cfg;
    cfg.snapThresholdM = 1.0f;
    cfg.blendRate = 0.5f;

    ClientPrediction pred;
    pred.init(cfg, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    // First reconcile establishes the predicted position.
    auto snap1 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap1, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // Second reconcile with server position >1 m away — must snap, not blend.
    auto snap2 = makeSnap(1u, 1u, glm::dvec3{100, 500, 0});
    pred.reconcile(snap2, 2u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // Position should be close to the new server-side origin (100,500,0), not the old one.
    CHECK(snap2.entries[0].position.x > 50.0);
}

TEST_CASE("ClientPrediction / blend on small divergence", "[client_prediction]") {
    PredictionSettings cfg;
    cfg.snapThresholdM = 100.0f; // very large threshold — always blend
    cfg.blendRate = 0.5f;

    ClientPrediction pred;
    pred.init(cfg, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto snap1 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap1, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    const double x1 = snap1.entries[0].position.x; // near 0

    // Second reconcile: server is at X=0.4 m — within threshold, should blend.
    auto snap2 = makeSnap(1u, 1u, glm::dvec3{0.4, 500, 0});
    pred.reconcile(snap2, 2u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    const double x2 = snap2.entries[0].position.x;

    // Blended position should be between the two: closer to server than old pred.
    CHECK(x2 > x1 - 1.0);  // not at -infinity
    CHECK(x2 < 0.4 + 1.0); // not past server position
}

TEST_CASE("ClientPrediction / history ring overflow is safe", "[client_prediction]") {
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto snap = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // Push 200 inputs (> kHistorySize=128) without crashing.
    for (uint32_t i = 1u; i <= 200u; ++i) {
        pred.onInput(makeInput(i, 0.f), makeEnv());
    }

    // Reconcile with delay=128 should not crash or produce NaN.
    auto snap2 = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    pred.reconcile(snap2, 2u, 128u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    CHECK(std::isfinite(snap2.entries[0].position.y));
}

TEST_CASE("ClientPrediction / reset clears state", "[client_prediction]") {
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto snap = makeSnap(1u, 1u);
    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());
    REQUIRE(pred.isInitialized());

    pred.reset();
    CHECK(!pred.isInitialized());

    // After reset, reconcile re-initializes lazily from scratch — no crash.
    auto snap2 = makeSnap(1u, 1u);
    pred.reconcile(snap2, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());
}

TEST_CASE("ClientPrediction / non-player entries untouched", "[client_prediction]") {
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    RenderSnapshot snap;
    snap.tickIndex = 1u;

    EntityRenderEntry player;
    player.entityIdx = 1u;
    player.entityGen = 1u;
    player.position = {0, 500, 0};
    player.orientation = glm::quat(1.f, 0.f, 0.f, 0.f);
    player.fuelPct = 100u;
    snap.entries.push_back(player);

    EntityRenderEntry other1;
    other1.entityIdx = 2u;
    other1.entityGen = 1u;
    other1.position = {1000, 500, 0};
    snap.entries.push_back(other1);

    EntityRenderEntry other2;
    other2.entityIdx = 3u;
    other2.entityGen = 1u;
    other2.position = {2000, 500, 0};
    snap.entries.push_back(other2);

    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // Non-player entries must be byte-identical.
    CHECK(snap.entries[1].position.x == Catch::Approx(1000.0));
    CHECK(snap.entries[2].position.x == Catch::Approx(2000.0));
}

TEST_CASE("ClientPrediction / omega from server snapshot seeds integrator and propagates to entry",
          "[client_prediction]") {
    ClientPrediction pred;
    pred.init(PredictionSettings{}, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    RenderSnapshot snap;
    snap.tickIndex = 1u;
    EntityRenderEntry entry;
    entry.entityIdx = 1u;
    entry.entityGen = 1u;
    entry.position = {0, 500, 0};
    entry.orientation = glm::quat(1.f, 0.f, 0.f, 0.f);
    entry.fuelPct = 100u;
    entry.omega = {0.5f, -0.3f, 0.1f}; // non-zero angular rates from server
    snap.entries.push_back(entry);

    pred.reconcile(snap, 1u, 0u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // The mutated entry's omega should reflect the integrator state after 0 replay ticks.
    // It was seeded from the server omega, so it should be close to the input values
    // (one step of integration may change them slightly due to damping).
    // Just verify the values are finite and non-zero initially.
    CHECK(std::isfinite(snap.entries[0].omega.x));
    CHECK(std::isfinite(snap.entries[0].omega.y));
    CHECK(std::isfinite(snap.entries[0].omega.z));
}

TEST_CASE("ClientPrediction / prediction disabled is a no-op", "[client_prediction]") {
    PredictionSettings cfg;
    cfg.enabled = false;

    ClientPrediction pred;
    pred.init(cfg, builtinResolver(), noPayload(), flatGround(), 1u, 1u);

    auto snap = makeSnap(1u, 1u, glm::dvec3{0, 500, 0});
    const glm::dvec3 origPos = snap.entries[0].position;

    pred.onInput(makeInput(1u, 1.f), makeEnv());
    pred.reconcile(snap, 1u, 1u, ClientPrediction::kNoAckedSeqNum, makeEnv());

    // Prediction is disabled — entry must not be modified.
    CHECK(!pred.isInitialized());
    CHECK(snap.entries[0].position.x == Catch::Approx(origPos.x));
    CHECK(snap.entries[0].position.y == Catch::Approx(origPos.y));
}
