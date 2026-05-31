// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"

#include <thread>

using namespace fl;

// ---------------------------------------------------------------------------
// RenderSnapshot defaults
// ---------------------------------------------------------------------------

TEST_CASE("RenderSnapshot defaults to tick zero with no entries") {
    RenderSnapshot snap{};
    CHECK(snap.tickIndex == 0);
    CHECK(snap.entries.empty());
}

TEST_CASE("EntityRenderEntry defaults to valid identity orientation") {
    EntityRenderEntry e{};
    CHECK(e.entityIdx == 0);
    CHECK(e.entityGen == 0);
    CHECK(e.typeIndex == 0);
    CHECK(e.damageLevel == 0);
    CHECK(e.playerOwned == false);
    // Identity quaternion: w=1, x=y=z=0
    CHECK(e.orientation.w == 1.0f);
    CHECK(e.orientation.x == 0.0f);
    CHECK(e.orientation.y == 0.0f);
    CHECK(e.orientation.z == 0.0f);
}

// ---------------------------------------------------------------------------
// SimRenderBridge initial state
// ---------------------------------------------------------------------------

TEST_CASE("SimRenderBridge has no snapshot before first publish") {
    SimRenderBridge bridge;
    CHECK_FALSE(bridge.hasSnapshot());
    CHECK_FALSE(bridge.tryAdvance());
}

TEST_CASE("SimRenderBridge current is valid even before first publish") {
    SimRenderBridge bridge;
    // current() is always safe to call; returns the empty default snapshot
    CHECK(bridge.current().tickIndex == 0);
    CHECK(bridge.current().entries.empty());
}

// ---------------------------------------------------------------------------
// Single publish / advance cycle
// ---------------------------------------------------------------------------

TEST_CASE("SimRenderBridge publish then tryAdvance delivers snapshot") {
    SimRenderBridge bridge;

    RenderSnapshot snap;
    snap.tickIndex = 7;
    EntityRenderEntry e;
    e.entityIdx = 3;
    e.entityGen = 1;
    e.typeIndex = 0;
    e.position = {1.0, 2.0, 3.0};
    e.damageLevel = 0;
    snap.entries.push_back(e);

    bridge.publish(std::move(snap));

    CHECK(bridge.hasSnapshot());
    REQUIRE(bridge.tryAdvance());
    CHECK(bridge.current().tickIndex == 7);
    REQUIRE(bridge.current().entries.size() == 1);
    CHECK(bridge.current().entries[0].entityIdx == 3);
    CHECK(bridge.current().entries[0].position.x == 1.0);
}

TEST_CASE("SimRenderBridge tryAdvance returns false when no new snapshot since last advance") {
    SimRenderBridge bridge;

    RenderSnapshot snap;
    snap.tickIndex = 1;
    bridge.publish(std::move(snap));

    CHECK(bridge.tryAdvance());       // advances to tick 1
    CHECK_FALSE(bridge.tryAdvance()); // no new publish since then
}

// ---------------------------------------------------------------------------
// Multiple publishes — always get latest
// ---------------------------------------------------------------------------

TEST_CASE("SimRenderBridge multiple publishes deliver latest tick") {
    SimRenderBridge bridge;

    for (uint64_t tick = 1; tick <= 5; ++tick) {
        RenderSnapshot snap;
        snap.tickIndex = tick;
        bridge.publish(std::move(snap));
    }

    REQUIRE(bridge.tryAdvance());
    // May have skipped intermediate ticks (triple-buffer always latest)
    CHECK(bridge.current().tickIndex >= 1u);
    CHECK(bridge.current().tickIndex <= 5u);
}

TEST_CASE("SimRenderBridge publish-advance-publish-advance round trips") {
    SimRenderBridge bridge;

    RenderSnapshot snap1;
    snap1.tickIndex = 10;
    bridge.publish(std::move(snap1));
    REQUIRE(bridge.tryAdvance());
    CHECK(bridge.current().tickIndex == 10);

    RenderSnapshot snap2;
    snap2.tickIndex = 11;
    bridge.publish(std::move(snap2));
    REQUIRE(bridge.tryAdvance());
    CHECK(bridge.current().tickIndex == 11);

    CHECK_FALSE(bridge.tryAdvance()); // nothing new
}

// ---------------------------------------------------------------------------
// Entry data integrity
// ---------------------------------------------------------------------------

TEST_CASE("SimRenderBridge preserves all EntityRenderEntry fields") {
    SimRenderBridge bridge;

    RenderSnapshot snap;
    snap.tickIndex = 42;

    EntityRenderEntry e;
    e.entityIdx = 99;
    e.entityGen = 3;
    e.typeIndex = 7;
    e.position = {10.0, 20.0, 30.0};
    e.orientation = glm::quat(0.707f, 0.0f, 0.707f, 0.0f);
    e.velocity = {5.0f, 0.0f, -3.0f};
    e.damageLevel = 2;
    e.playerOwned = true;
    snap.entries.push_back(e);

    bridge.publish(std::move(snap));
    REQUIRE(bridge.tryAdvance());

    REQUIRE(bridge.current().entries.size() == 1);
    const auto& got = bridge.current().entries[0];
    CHECK(got.entityIdx == 99);
    CHECK(got.entityGen == 3);
    CHECK(got.typeIndex == 7);
    CHECK(got.position.x == 10.0);
    CHECK(got.position.y == 20.0);
    CHECK(got.position.z == 30.0);
    CHECK(got.velocity.z == -3.0f);
    CHECK(got.damageLevel == 2);
    CHECK(got.playerOwned == true);
}

// ---------------------------------------------------------------------------
// Multi-entry snapshot
// ---------------------------------------------------------------------------

TEST_CASE("SimRenderBridge snapshot with multiple entries preserves all") {
    SimRenderBridge bridge;

    RenderSnapshot snap;
    snap.tickIndex = 1;
    for (uint32_t i = 0; i < 8; ++i) {
        EntityRenderEntry e;
        e.entityIdx = i;
        e.entityGen = 1;
        e.position = {static_cast<double>(i), 0.0, 0.0};
        snap.entries.push_back(e);
    }

    bridge.publish(std::move(snap));
    REQUIRE(bridge.tryAdvance());
    REQUIRE(bridge.current().entries.size() == 8);
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(bridge.current().entries[i].entityIdx == i);
        CHECK(bridge.current().entries[i].position.x == static_cast<double>(i));
    }
}

// ---------------------------------------------------------------------------
// Threaded smoke test — sim publishes while render consumes
// ---------------------------------------------------------------------------

TEST_CASE("SimRenderBridge concurrent publish and advance does not deadlock") {
    SimRenderBridge bridge;

    constexpr int kTicks = 200;

    std::thread simThread([&bridge]() {
        for (int i = 0; i < kTicks; ++i) {
            RenderSnapshot snap;
            snap.tickIndex = static_cast<uint64_t>(i + 1);
            EntityRenderEntry e;
            e.entityIdx = static_cast<uint32_t>(i);
            e.position = {static_cast<double>(i), 0.0, 0.0};
            snap.entries.push_back(e);
            bridge.publish(std::move(snap));
        }
    });

    uint64_t lastTick = 0;
    for (int frame = 0; frame < kTicks * 2; ++frame) {
        if (bridge.tryAdvance()) {
            uint64_t t = bridge.current().tickIndex;
            CHECK(t >= lastTick); // monotone: never goes backwards
            lastTick = t;
        }
    }

    simThread.join();

    // Drain any remaining snapshot published before the thread exited.
    bridge.tryAdvance();
    CHECK(bridge.current().tickIndex <= static_cast<uint64_t>(kTicks));
}

TEST_CASE("SimRenderBridge publishExternal delivers snapshot on same thread", "[bridge]") {
    SimRenderBridge bridge;
    CHECK(!bridge.hasSnapshot());

    RenderSnapshot snap;
    snap.tickIndex = 77u;
    EntityRenderEntry e;
    e.entityIdx = 42u;
    snap.entries.push_back(e);

    // publishExternal + tryAdvance from the same thread (network-client mode: no sim thread).
    bridge.publishExternal(std::move(snap));
    CHECK(bridge.hasSnapshot());
    REQUIRE(bridge.tryAdvance());
    CHECK(bridge.current().tickIndex == 77u);
    REQUIRE(bridge.current().entries.size() == 1u);
    CHECK(bridge.current().entries[0].entityIdx == 42u);

    // A second tryAdvance with no new publish returns false.
    CHECK(!bridge.tryAdvance());
}

TEST_CASE("SimRenderBridge preserves double precision at planet-scale coordinates", "[bridge]") {
    // At 2,000 km from origin float32 precision degrades to ~0.24 m; dvec3 must survive exact.
    constexpr double kLarge = 2'000'000.0;

    SimRenderBridge bridge;
    RenderSnapshot snap;
    snap.tickIndex = 1;
    EntityRenderEntry e;
    e.position = {kLarge, 0.0, kLarge};
    snap.entries.push_back(e);

    bridge.publish(std::move(snap));
    REQUIRE(bridge.tryAdvance());

    CHECK(bridge.current().entries[0].position.x == kLarge);
    CHECK(bridge.current().entries[0].position.z == kLarge);
}
