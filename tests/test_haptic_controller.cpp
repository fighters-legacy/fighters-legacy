// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "HapticController.h"
#include "mock_hal.h"

// ---------------------------------------------------------------------------
// Tracking mock — records every haptic call for inspection.
// ---------------------------------------------------------------------------
struct TrackingInput : MockInput {
    struct RumbleCall {
        float lo, hi;
        uint32_t ms;
    };
    struct TriggerCall {
        float left, right;
        uint32_t ms;
    };

    std::vector<RumbleCall> rumbleCalls;
    std::vector<TriggerCall> triggerCalls;
    int stopCount{0};
    bool rumbleSupported{true};
    bool triggerSupported{true};

    void rumble(int, float lo, float hi, uint32_t ms) override {
        rumbleCalls.push_back({lo, hi, ms});
    }
    void rumbleTriggers(int, float l, float r, uint32_t ms) override {
        triggerCalls.push_back({l, r, ms});
    }
    bool supportsRumble(int) const override {
        return rumbleSupported;
    }
    bool supportsTriggerRumble(int) const override {
        return triggerSupported;
    }
    void stopRumble(int) override {
        ++stopCount;
    }

    void clear() {
        rumbleCalls.clear();
        triggerCalls.clear();
        stopCount = 0;
    }
};

// ---------------------------------------------------------------------------
// Helper: build a minimal EntityRenderEntry at safe altitude (no GPWS/touchdown).
// ---------------------------------------------------------------------------
static fl::EntityRenderEntry makeEntry(uint8_t throttle = 50, uint8_t damage = 0, glm::vec3 vel = {}) {
    fl::EntityRenderEntry e{};
    e.throttle = throttle;
    e.damageLevel = damage;
    e.velocity = vel;
    e.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    e.position = {0.0, 5000.0, 0.0};
    return e;
}

static constexpr float kDt = 1.0f / 60.0f;

// ===========================================================================
// Gun burst
// ===========================================================================
TEST_CASE("gun burst fires on weapon fired") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    hc.update(&e, true, 0.0f, kDt);
    REQUIRE(!inp.rumbleCalls.empty());
    const auto& c = inp.rumbleCalls.back();
    CHECK(c.lo == Catch::Approx(0.0f));
    CHECK(c.hi == Catch::Approx(0.8f));
    CHECK(c.ms == 80u);
}

TEST_CASE("gun burst does not fire when weapon not fired") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    hc.update(&e, false, 0.0f, kDt);
    // Filter to calls matching gun burst signature (lo=0, hi=0.8, ms=80)
    int gunCalls = 0;
    for (auto& c : inp.rumbleCalls)
        if (c.hi == Catch::Approx(0.8f) && c.ms == 80u)
            ++gunCalls;
    CHECK(gunCalls == 0);
}

// ===========================================================================
// Hit taken
// ===========================================================================
TEST_CASE("hit taken fires on damage level increase") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0);
    hc.update(&e, false, 0.0f, kDt);
    inp.clear();
    e.damageLevel = 1;
    hc.update(&e, false, 0.0f, kDt);
    REQUIRE(!inp.rumbleCalls.empty());
    const auto& c = inp.rumbleCalls.front();
    CHECK(c.lo == Catch::Approx(0.8f));
    CHECK(c.hi == Catch::Approx(0.4f));
    CHECK(c.ms == 120u);
}

TEST_CASE("hit taken does not fire on unchanged damage level") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 1);
    hc.update(&e, false, 0.0f, kDt);
    inp.clear();
    hc.update(&e, false, 0.0f, kDt);
    // No lo=0.8, hi=0.4 calls expected
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.8f) && c.hi == Catch::Approx(0.4f) && c.ms == 120u));
}

// ===========================================================================
// Stall buffet
// ===========================================================================
TEST_CASE("stall buffet fires while above stall AoA") {
    // vel={10,-4,0} with identity orientation: alpha = atan2(4,10) ~21.8 deg > 18 deg
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {10.0f, -4.0f, 0.0f});
    hc.update(&e, false, 0.0f, kDt);
    REQUIRE(!inp.rumbleCalls.empty());
    CHECK(inp.rumbleCalls.front().lo == Catch::Approx(0.3f));
    CHECK(inp.rumbleCalls.front().hi == Catch::Approx(0.1f));
    CHECK(inp.rumbleCalls.front().ms == 200u);
    // Verify re-fire after timer expires
    inp.clear();
    hc.update(&e, false, 0.0f, 0.2f); // advance past 0.15s re-fire interval
    REQUIRE(!inp.rumbleCalls.empty());
}

TEST_CASE("stall buffet stops when AoA drops below threshold") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {10.0f, -4.0f, 0.0f});
    hc.update(&e, false, 0.0f, kDt);
    inp.clear();
    e.velocity = {10.0f, 0.0f, 0.0f}; // AoA = 0 deg
    hc.update(&e, false, 0.0f, 0.2f);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.3f) && c.hi == Catch::Approx(0.1f)));
}

// ===========================================================================
// Afterburner
// ===========================================================================
TEST_CASE("afterburner ignition fires on throttle 100 transition") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0);
    hc.update(&e, false, 0.0f, kDt); // establish m_abActive=false
    inp.clear();
    e.throttle = 100;
    hc.update(&e, false, 0.0f, kDt);
    bool found = false;
    for (auto& c : inp.rumbleCalls)
        if (c.lo == Catch::Approx(0.4f) && c.hi == Catch::Approx(0.2f) && c.ms == 300u)
            found = true;
    CHECK(found);
}

TEST_CASE("afterburner sustain re-fires while throttle stays at 100") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(100, 0);
    hc.update(&e, false, 0.0f, 0.02f); // ignition on first frame
    size_t callsAfterIgnition = inp.rumbleCalls.size();
    hc.update(&e, false, 0.0f, 0.3f); // advance past 0.25s sustain delay
    CHECK(inp.rumbleCalls.size() > callsAfterIgnition);
}

TEST_CASE("afterburner sustain stops when throttle drops") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(100, 0);
    hc.update(&e, false, 0.0f, 0.02f); // ignition
    e.throttle = 50;
    hc.update(&e, false, 0.0f, 0.02f); // AB off
    inp.clear();
    hc.update(&e, false, 0.0f, 0.5f); // no sustain expected
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.15f) && c.hi == Catch::Approx(0.08f)));
}

// ===========================================================================
// Engine failure
// ===========================================================================
TEST_CASE("engine failure re-fires while damage level is 2 or above") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 2);
    hc.update(&e, false, 0.0f, 0.5f); // first frame — hit-taken fires; engine fail fires
    inp.clear();
    // From here prevDamageLevel == 2; only engine failure re-fires expected
    hc.update(&e, false, 0.0f, 0.5f);
    hc.update(&e, false, 0.0f, 0.5f);
    CHECK(inp.rumbleCalls.size() >= 2u);
    for (auto& c : inp.rumbleCalls)
        CHECK(c.lo == Catch::Approx(0.5f));
}

// ===========================================================================
// G-LOC onset
// ===========================================================================
TEST_CASE("G-LOC fires with proportional intensity above threshold") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {0.0f, 0.0f, 0.0f});
    // First frame — establish prevVelocity = {0,0,0}, firstFrame cleared
    hc.update(&e, false, 0.0f, kDt);
    inp.clear();
    // Delta={20,0,0}: gLoad = 20 / (kDt * 9.81) >> 6 G
    e.velocity = {20.0f, 0.0f, 0.0f};
    hc.update(&e, false, 0.0f, kDt);
    REQUIRE(!inp.rumbleCalls.empty());
    const auto& c = inp.rumbleCalls.back();
    CHECK(c.lo == Catch::Approx(0.0f));
    CHECK(c.hi > 0.0f);
}

TEST_CASE("G-LOC is skipped on the first frame to prevent velocity-init spike") {
    TrackingInput inp;
    HapticController hc(inp);
    // Large velocity on frame 0 — m_firstFrame guard must suppress G-LOC
    auto e = makeEntry(50, 0, {500.0f, 0.0f, 0.0f});
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.0f) && c.hi > 0.0f && c.ms == 200u));
}

TEST_CASE("G-LOC does not fire when velocity delta is below threshold") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {0.0f, 0.0f, 0.0f});
    hc.update(&e, false, 0.0f, kDt); // clear firstFrame
    inp.clear();
    // Tiny delta — well below 6G
    e.velocity = {0.001f, 0.0f, 0.0f};
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.0f) && c.hi > 0.0f && c.ms == 200u));
}

// ===========================================================================
// Transonic buffet
// ===========================================================================
TEST_CASE("transonic buffet fires at Mach 0.9") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {306.27f, 0.0f, 0.0f}); // ~Mach 0.9
    hc.update(&e, false, 0.0f, kDt);
    bool found = false;
    for (auto& c : inp.rumbleCalls)
        if (c.lo == Catch::Approx(0.3f) && c.hi == Catch::Approx(0.3f) && c.ms == 400u)
            found = true;
    CHECK(found);
}

TEST_CASE("transonic buffet does not fire below Mach 0.85") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {238.21f, 0.0f, 0.0f}); // ~Mach 0.7
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.3f) && c.hi == Catch::Approx(0.3f) && c.ms == 400u));
}

TEST_CASE("transonic buffet stops when Mach exits range above 1.05") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {306.27f, 0.0f, 0.0f}); // in range
    hc.update(&e, false, 0.0f, kDt);
    inp.clear();
    e.velocity = {374.33f, 0.0f, 0.0f}; // ~Mach 1.1, out of range
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.3f) && c.hi == Catch::Approx(0.3f) && c.ms == 400u));
}

// ===========================================================================
// Landing gear touchdown
// ===========================================================================
TEST_CASE("touchdown fires on AGL transition from above 2m to below 2m") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    e.position = {0.0, 5.0, 0.0};    // agl = 5m
    hc.update(&e, false, 0.0f, kDt); // sets prevAgl = 5.0
    inp.clear();
    e.position = {0.0, 1.0, 0.0}; // agl = 1m
    hc.update(&e, false, 0.0f, kDt);
    bool found = false;
    for (auto& c : inp.rumbleCalls)
        if (c.lo == Catch::Approx(0.9f) && c.hi == Catch::Approx(0.3f) && c.ms == 200u)
            found = true;
    CHECK(found);
}

TEST_CASE("touchdown does not double-fire when staying on ground") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    e.position = {0.0, 5.0, 0.0};
    hc.update(&e, false, 0.0f, kDt); // prevAgl = 5.0
    e.position = {0.0, 0.5, 0.0};
    hc.update(&e, false, 0.0f, kDt); // fires touchdown, prevAgl = 0.5
    inp.clear();
    hc.update(&e, false, 0.0f, kDt); // prevAgl = 0.5 < 2.0 → no re-trigger
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.9f) && c.hi == Catch::Approx(0.3f) && c.ms == 200u));
}

TEST_CASE("touchdown does not fire on first frame when already at ground level") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    e.position = {0.0, 0.5, 0.0}; // agl = 0.5, prevAgl = 0 → 0 < 2 → no trigger
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.9f) && c.hi == Catch::Approx(0.3f) && c.ms == 200u));
}

// ===========================================================================
// GPWS / terrain warning
// ===========================================================================
TEST_CASE("GPWS fires double pulse when low and descending") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    e.position = {0.0, 200.0, 0.0}; // agl = 200 < 300
    e.velocity = {0.0f, -10.0f, 0.0f};
    hc.update(&e, false, 0.0f, kDt);   // Idle → Pulse1 fires, phase=Pulse1, timer=0.1
    hc.update(&e, false, 0.0f, 0.12f); // Pulse1 expires → Gap, timer=0.15
    hc.update(&e, false, 0.0f, 0.2f);  // Gap expires → Pulse2 fires, phase=Pulse2
    int pulseCalls = 0;
    for (auto& c : inp.rumbleCalls)
        if (c.lo == Catch::Approx(0.5f) && c.hi == Catch::Approx(0.5f) && c.ms == 100u)
            ++pulseCalls;
    CHECK(pulseCalls == 2);
}

TEST_CASE("GPWS cooldown prevents immediate re-trigger") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    e.position = {0.0, 200.0, 0.0};
    e.velocity = {0.0f, -10.0f, 0.0f};
    // Drive through full sequence to reach Cooldown
    hc.update(&e, false, 0.0f, kDt);
    hc.update(&e, false, 0.0f, 0.12f);
    hc.update(&e, false, 0.0f, 0.2f);
    hc.update(&e, false, 0.0f, 0.12f); // into Cooldown
    inp.clear();
    // Immediate re-trigger attempt — should not fire (still in Cooldown)
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.5f) && c.hi == Catch::Approx(0.5f) && c.ms == 100u));
}

TEST_CASE("GPWS does not fire when ascending") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    e.position = {0.0, 200.0, 0.0};
    e.velocity = {0.0f, 5.0f, 0.0f}; // positive vertical velocity
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.5f) && c.hi == Catch::Approx(0.5f) && c.ms == 100u));
}

TEST_CASE("GPWS does not fire above altitude threshold") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry();
    e.position = {0.0, 400.0, 0.0}; // agl = 400 > 300
    e.velocity = {0.0f, -10.0f, 0.0f};
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.5f) && c.hi == Catch::Approx(0.5f) && c.ms == 100u));
}

// ===========================================================================
// onPause
// ===========================================================================
TEST_CASE("onPause calls stopRumble") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.onPause(0);
    CHECK(inp.stopCount == 1);
}

TEST_CASE("onPause resets stall timer so no immediate re-fire after pause") {
    TrackingInput inp;
    HapticController hc(inp);
    auto e = makeEntry(50, 0, {10.0f, -4.0f, 0.0f}); // stall AoA
    hc.update(&e, false, 0.0f, kDt);                 // timer fires and resets to 0.15
    hc.onPause(0);
    inp.clear();
    // Non-stall update immediately after pause — timer was reset by onPause, no fire
    e.velocity = {10.0f, 0.0f, 0.0f}; // AoA = 0
    hc.update(&e, false, 0.0f, kDt);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.3f) && c.hi == Catch::Approx(0.1f)));
}

// ===========================================================================
// Stub event methods
// ===========================================================================
TEST_CASE("notifyMissileLaunch fires correct rumble") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.notifyMissileLaunch();
    REQUIRE(!inp.rumbleCalls.empty());
    const auto& c = inp.rumbleCalls.front();
    CHECK(c.lo == Catch::Approx(0.6f));
    CHECK(c.hi == Catch::Approx(0.6f));
    CHECK(c.ms == 150u);
}

TEST_CASE("notifyMissileWarning fires low-frequency rumble") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.notifyMissileWarning();
    REQUIRE(!inp.rumbleCalls.empty());
    CHECK(inp.rumbleCalls.front().lo > 0.0f);
    CHECK(inp.rumbleCalls.front().hi == Catch::Approx(0.0f));
}

TEST_CASE("notifyOrdnanceRelease fires correct rumble") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.notifyOrdnanceRelease();
    REQUIRE(!inp.rumbleCalls.empty());
    const auto& c = inp.rumbleCalls.front();
    CHECK(c.lo == Catch::Approx(0.4f));
    CHECK(c.hi == Catch::Approx(0.0f));
    CHECK(c.ms == 80u);
}

TEST_CASE("notifyCarrierTrap fires trigger rumble when supported") {
    TrackingInput inp;
    inp.triggerSupported = true;
    HapticController hc(inp);
    hc.notifyCarrierTrap();
    REQUIRE(!inp.triggerCalls.empty());
    CHECK(inp.triggerCalls.front().left == Catch::Approx(0.9f));
    CHECK(inp.triggerCalls.front().right == Catch::Approx(0.9f));
}

TEST_CASE("notifyCarrierTrap does not fire when trigger rumble unsupported") {
    TrackingInput inp;
    inp.triggerSupported = false;
    HapticController hc(inp);
    hc.notifyCarrierTrap();
    CHECK(inp.triggerCalls.empty());
}

// ===========================================================================
// Hydraulic failure
// ===========================================================================
TEST_CASE("hydraulic failure fires rumble when active with input") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.notifyHydraulicFailure(true, true);
    auto e = makeEntry();
    hc.update(&e, false, 0.0f, 0.5f); // timer: 0-0.5 <= 0 → fires
    bool found = false;
    for (auto& c : inp.rumbleCalls)
        if (c.lo == Catch::Approx(0.2f) && c.hi == Catch::Approx(0.0f))
            found = true;
    CHECK(found);
}

TEST_CASE("hydraulic failure does not fire when active but no input") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.notifyHydraulicFailure(true, false);
    auto e = makeEntry();
    hc.update(&e, false, 0.0f, 0.5f);
    for (auto& c : inp.rumbleCalls)
        CHECK_FALSE((c.lo == Catch::Approx(0.2f) && c.hi == Catch::Approx(0.0f)));
}

// ===========================================================================
// Compressor stall sequence
// ===========================================================================
TEST_CASE("compressor stall delivers exactly 4 pulses") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.notifyCompressorStall();
    auto e = makeEntry();
    // Drive through all 4 pulses with dt large enough to expire each gap
    for (int i = 0; i < 8; ++i)
        hc.update(&e, false, 0.0f, 0.1f);
    int pulses = 0;
    for (auto& c : inp.rumbleCalls)
        if (c.lo == Catch::Approx(0.6f) && c.hi == Catch::Approx(0.0f))
            ++pulses;
    CHECK(pulses == 4);
}

TEST_CASE("second compressor stall call mid-sequence is a no-op") {
    TrackingInput inp;
    HapticController hc(inp);
    hc.notifyCompressorStall();
    auto e = makeEntry();
    hc.update(&e, false, 0.0f, 0.1f); // fires pulse 0
    hc.notifyCompressorStall();       // ignored — already running
    for (int i = 0; i < 7; ++i)
        hc.update(&e, false, 0.0f, 0.1f);
    int pulses = 0;
    for (auto& c : inp.rumbleCalls)
        if (c.lo == Catch::Approx(0.6f) && c.hi == Catch::Approx(0.0f))
            ++pulses;
    CHECK(pulses == 4);
}

// ===========================================================================
// Capability guard
// ===========================================================================
TEST_CASE("no rumble calls when controller has no haptic support") {
    TrackingInput inp;
    inp.rumbleSupported = false;
    inp.triggerSupported = false;
    HapticController hc(inp);

    auto e = makeEntry(100, 2, {306.27f, -4.0f, 0.0f}); // all triggers at once
    e.position = {0.0, 200.0, 0.0};
    hc.update(&e, true, 0.0f, kDt);
    hc.update(&e, true, 0.0f, 0.5f);

    hc.notifyMissileLaunch();
    hc.notifyMissileWarning();
    hc.notifyOrdnanceRelease();
    hc.notifyCompressorStall();
    hc.notifyCarrierTrap();
    hc.notifyHydraulicFailure(true, true);
    hc.update(&e, false, 0.0f, 0.5f);

    CHECK(inp.rumbleCalls.empty());
    CHECK(inp.triggerCalls.empty());
}
