// SPDX-License-Identifier: GPL-3.0-or-later
//
// TransformHistory (#425): the lag-compensation rewind ring. The cases that matter are the ones
// that must fail closed — an aged-out tick, a never-recorded tick, and above all a RECYCLED pool
// slot: history must never resolve onto an entity that did not exist at the rewound tick.
#include <catch2/catch_test_macros.hpp>

#include "net/TransformHistory.h"

using namespace fl;

namespace {
void record(TransformHistory& h, uint64_t tick, uint32_t idx, uint16_t gen, double x) {
    const double pos[3] = {x, 500.0, 0.0};
    h.add(tick, idx, gen, pos);
}
} // namespace

TEST_CASE("TransformHistory: records and returns a position for the right tick and generation", "[transform_history]") {
    TransformHistory h;
    h.beginTick(10);
    record(h, 10, 3, 1, 42.0);

    const auto p = h.queryAt(10, 3, 1);
    REQUIRE(p.has_value());
    CHECK(p->x == 42.0);
    CHECK(p->y == 500.0);

    CHECK_FALSE(h.queryAt(10, 4, 1).has_value()); // entity never recorded that tick
    CHECK_FALSE(h.queryAt(9, 3, 1).has_value());  // tick never recorded at all
    CHECK(h.hasTick(10));
    CHECK_FALSE(h.hasTick(9));
}

TEST_CASE("TransformHistory: a recycled slot's old generation is unreachable", "[transform_history]") {
    TransformHistory h;
    h.beginTick(5);
    record(h, 5, 7, 2, 10.0); // entity gen 2 lived in slot 7 at tick 5

    // The entity died; slot 7 was reused by gen 3. Querying tick 5 with the NEW generation must
    // miss — the new entity did not exist then, and hitting the corpse's position would damage it.
    CHECK_FALSE(h.queryAt(5, 7, 3).has_value());
    CHECK(h.queryAt(5, 7, 2).has_value()); // the generation that was actually there still resolves
}

TEST_CASE("TransformHistory: the ring wraps and old ticks age out", "[transform_history]") {
    TransformHistory h;
    for (uint64_t t = 1; t <= TransformHistory::kHistoryTicks + 1; ++t) {
        h.beginTick(t);
        record(h, t, 1, 1, static_cast<double>(t));
    }

    // Tick 1 was overwritten by tick kHistoryTicks+1 (same ring slot).
    CHECK_FALSE(h.queryAt(1, 1, 1).has_value());
    CHECK_FALSE(h.hasTick(1));
    const auto newest = h.queryAt(TransformHistory::kHistoryTicks + 1, 1, 1);
    REQUIRE(newest.has_value());
    CHECK(newest->x == static_cast<double>(TransformHistory::kHistoryTicks + 1));
    // Tick 2 is still resident (the ring holds the last kHistoryTicks ticks).
    CHECK(h.queryAt(2, 1, 1).has_value());
}

TEST_CASE("TransformHistory: beginTick clears the reused slot's stale entries", "[transform_history]") {
    TransformHistory h;
    h.beginTick(1);
    record(h, 1, 9, 1, 1.0);
    h.beginTick(1 + TransformHistory::kHistoryTicks); // same slot, new tick, no entities recorded

    CHECK_FALSE(h.queryAt(1 + TransformHistory::kHistoryTicks, 9, 1).has_value());
    CHECK_FALSE(h.queryAt(1, 9, 1).has_value());
}

TEST_CASE("TransformHistory: add without beginTick is refused, not misfiled", "[transform_history]") {
    TransformHistory h;
    record(h, 3, 1, 1, 5.0); // no beginTick(3): must not poison whatever slot 3 hashes to
    CHECK_FALSE(h.queryAt(3, 1, 1).has_value());
}

TEST_CASE("TransformHistory: clear empties every slot", "[transform_history]") {
    TransformHistory h;
    h.beginTick(4);
    record(h, 4, 1, 1, 5.0);
    h.clear();
    CHECK_FALSE(h.queryAt(4, 1, 1).has_value());
    CHECK_FALSE(h.hasTick(4));
}
