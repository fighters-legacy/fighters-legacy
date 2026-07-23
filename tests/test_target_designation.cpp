// SPDX-License-Identifier: GPL-3.0-or-later
//
// Client-side target designation tests (#696): cycle ordering, category/self/destroyed filtering,
// generation-stale + despawn auto-clear, best-in-cone selection with nearest fallback, and the
// provider-swap seam. Pure logic over EntityRenderEntry / RenderSnapshot.

#include "TargetDesignation.h"

#include "entity/DamageDef.h"
#include "entity/ObjectCategory.h"
#include "render/RadarView.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

using namespace fl;

namespace {

EntityRenderEntry mk(uint32_t idx, uint32_t gen, glm::dvec3 pos, uint32_t typeIndex = 0, uint8_t dmg = 0) {
    EntityRenderEntry e;
    e.entityIdx = idx;
    e.entityGen = gen;
    e.position = pos;
    e.typeIndex = typeIndex;
    e.damageLevel = dmg;
    return e;
}

// typeIndex encodes the category directly for the tests.
DesignationContext ctxFor(const RenderSnapshot& snap, uint32_t ownIdx = 99, uint32_t ownGen = 1,
                          glm::vec3 fwd = {0, 0, -1}) {
    DesignationContext ctx;
    ctx.snap = &snap;
    ctx.ownIdx = ownIdx;
    ctx.ownGen = ownGen;
    ctx.ownPos = {0, 0, 0};
    ctx.ownForward = fwd;
    ctx.categoryOf = [](uint32_t ti) { return static_cast<uint8_t>(ti); };
    ctx.identOf = [](const EntityRenderEntry& e) {
        // typeIndex 100 marks a hostile foe for the ordering test.
        return e.typeIndex == 100u ? kIffFoe : kIffUnknown;
    };
    return ctx;
}

constexpr uint8_t kAir = static_cast<uint8_t>(ObjectCategory::AirVehicle);
constexpr uint8_t kProjectile = static_cast<uint8_t>(ObjectCategory::Projectile);
constexpr uint8_t kEffect = static_cast<uint8_t>(ObjectCategory::Effect);
constexpr uint8_t kDestroyed = static_cast<uint8_t>(DamageLevel::Destroyed);

} // namespace

TEST_CASE("TargetDesignation: cycle skips self / projectiles / effects / destroyed (#696)", "[target_designation]") {
    RenderSnapshot snap;
    snap.entries.push_back(mk(1, 1, {0, 0, -1000}, kAir));            // valid, near
    snap.entries.push_back(mk(99, 1, {0, 0, -500}, kAir));            // OWN ship
    snap.entries.push_back(mk(2, 1, {0, 0, -200}, kProjectile));      // projectile -> skipped
    snap.entries.push_back(mk(3, 1, {0, 0, -300}, kEffect));          // effect -> skipped
    snap.entries.push_back(mk(4, 1, {0, 0, -400}, kAir, kDestroyed)); // destroyed -> skipped
    snap.entries.push_back(mk(5, 1, {0, 0, -2000}, kAir));            // valid, far

    TargetDesignation td;
    auto ctx = ctxFor(snap);
    td.cycle(+1, ctx);
    REQUIRE(td.designated());
    CHECK(td.idx() == 1u); // nearest valid
    td.cycle(+1, ctx);
    CHECK(td.idx() == 5u); // next valid
    td.cycle(+1, ctx);
    CHECK(td.idx() == 1u); // wraps back
}

TEST_CASE("TargetDesignation: hostiles are ordered before neutrals, then by range (#696)", "[target_designation]") {
    RenderSnapshot snap;
    snap.entries.push_back(mk(1, 1, {0, 0, -100}, kAir)); // neutral, very near
    snap.entries.push_back(mk(2, 1, {0, 0, -3000}, 100)); // hostile (typeIndex 100), far
    snap.entries.push_back(mk(3, 1, {0, 0, -1500}, 100)); // hostile, mid
    // Both 2 and 3 use typeIndex 100 -> category ordinal 100, which is targetable (not Projectile/Effect).

    TargetDesignation td;
    auto ctx = ctxFor(snap);
    td.cycle(+1, ctx);
    CHECK(td.idx() == 3u); // nearest HOSTILE first (hostiles before the nearer neutral)
    td.cycle(+1, ctx);
    CHECK(td.idx() == 2u); // farther hostile
    td.cycle(+1, ctx);
    CHECK(td.idx() == 1u); // then the neutral
}

TEST_CASE("TargetDesignation: previous cycles in reverse and wraps (#696)", "[target_designation]") {
    RenderSnapshot snap;
    snap.entries.push_back(mk(1, 1, {0, 0, -1000}, kAir));
    snap.entries.push_back(mk(2, 1, {0, 0, -2000}, kAir));
    TargetDesignation td;
    auto ctx = ctxFor(snap);
    td.cycle(-1, ctx);
    CHECK(td.idx() == 2u); // last
    td.cycle(-1, ctx);
    CHECK(td.idx() == 1u);
    td.cycle(-1, ctx);
    CHECK(td.idx() == 2u); // wraps
}

TEST_CASE("TargetDesignation: resolve auto-clears on despawn, generation mismatch, and death (#696)",
          "[target_designation]") {
    RenderSnapshot snap;
    snap.entries.push_back(mk(7, 3, {0, 0, -1000}, kAir));
    TargetDesignation td;
    td.cycle(+1, ctxFor(snap));
    REQUIRE(td.designated());
    REQUIRE(td.idx() == 7u);
    CHECK(td.resolve(snap) != nullptr);

    // Same idx, newer generation (pool slot reuse) -> handle no longer resolves.
    RenderSnapshot reused;
    reused.entries.push_back(mk(7, 4, {0, 0, -1000}, kAir));
    CHECK(td.resolve(reused) == nullptr);
    CHECK_FALSE(td.designated());

    // Re-designate, then the entity despawns entirely.
    td.cycle(+1, ctxFor(snap));
    RenderSnapshot empty;
    CHECK(td.resolve(empty) == nullptr);
    CHECK_FALSE(td.designated());

    // Re-designate, then it is destroyed in place.
    td.cycle(+1, ctxFor(snap));
    RenderSnapshot dead;
    dead.entries.push_back(mk(7, 3, {0, 0, -1000}, kAir, kDestroyed));
    CHECK(td.resolve(dead) == nullptr);
    CHECK_FALSE(td.designated());
}

TEST_CASE("TargetDesignation: designateBest prefers the in-cone target, falls back to nearest (#696)",
          "[target_designation]") {
    RenderSnapshot snap;
    snap.entries.push_back(mk(1, 1, {2000, 0, -100}, kAir)); // nearly abeam (wide cone), close-ish
    snap.entries.push_back(mk(2, 1, {0, 0, -5000}, kAir));   // dead ahead (in cone), far

    TargetDesignation td;
    auto ctx = ctxFor(snap, /*ownIdx=*/99, /*ownGen=*/1, /*fwd=*/{0, 0, -1});
    // A tight cone: only the dead-ahead entity (idx 2) qualifies, even though idx 1 is nearer.
    CHECK(td.designateBest(ctx, /*coneHalfAngleRad=*/0.2f));
    CHECK(td.idx() == 2u);

    // With nothing in a very tight cone... move both off-axis so the fallback (nearest) kicks in.
    RenderSnapshot snap2;
    snap2.entries.push_back(mk(1, 1, {5000, 0, -100}, kAir)); // near, off to the side
    snap2.entries.push_back(mk(2, 1, {9000, 0, -100}, kAir)); // far, off to the side
    auto ctx2 = ctxFor(snap2, 99, 1, {0, 0, -1});
    CHECK(td.designateBest(ctx2, 0.05f));
    CHECK(td.idx() == 1u); // nearest fallback

    // Empty snapshot -> nothing designated.
    RenderSnapshot empty;
    CHECK_FALSE(td.designateBest(ctxFor(empty), 0.2f));
    CHECK_FALSE(td.designated());
}

TEST_CASE("TargetDesignation: a swapped provider replaces the candidate source (#696)", "[target_designation]") {
    RenderSnapshot snap;
    snap.entries.push_back(mk(1, 1, {0, 0, -1000}, kAir));
    snap.entries.push_back(mk(2, 1, {0, 0, -2000}, kAir));

    TargetDesignation td;
    // A fake provider that only ever offers idx 2 (e.g. a future sensor-track source).
    td.setProvider([](const DesignationContext&, std::vector<TargetCandidate>& out) {
        out.clear();
        out.push_back(TargetCandidate{2, 1, 2000.0, false, 0.0f});
    });
    td.cycle(+1, ctxFor(snap));
    CHECK(td.idx() == 2u);
    td.cycle(+1, ctxFor(snap));
    CHECK(td.idx() == 2u); // the provider offers only one candidate

    // Restore the default provider.
    td.setProvider(nullptr);
    td.cycle(+1, ctxFor(snap));
    CHECK(td.idx() == 1u); // default scan: nearest first
}
