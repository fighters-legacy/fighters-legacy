// SPDX-License-Identifier: GPL-3.0-or-later
#include "RecordScheduler.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

TEST_CASE("RecordScheduler ticksPerFrame from fps", "[record-scheduler]") {
    CHECK(RecordScheduler(30).ticksPerFrame() == 2u); // 60/30
    CHECK(RecordScheduler(60).ticksPerFrame() == 1u);
    CHECK(RecordScheduler(15).ticksPerFrame() == 4u);
    CHECK(RecordScheduler(24).ticksPerFrame() == 3u);   // lround(60/24 = 2.5) = 3 (half away from zero)
    CHECK(RecordScheduler(1000).ticksPerFrame() == 1u); // clamped to >= 1
    CHECK(RecordScheduler(0).ticksPerFrame() == 2u);    // fps<=0 defaults to 30
}

TEST_CASE("RecordScheduler keeps pace with no dups when the tick stream advances by ticksPerFrame",
          "[record-scheduler]") {
    RecordScheduler s(30); // boundary every 2 ticks
    // First observed tick anchors the grid and emits the first frame.
    CHECK(s.boundariesDue(100) == 1);
    CHECK(s.totalFrames() == 1u);
    CHECK(s.dupFrames() == 0u);
    // Advance exactly one boundary each iteration -> one fresh frame, no dups.
    CHECK(s.boundariesDue(102) == 1);
    CHECK(s.boundariesDue(104) == 1);
    CHECK(s.boundariesDue(106) == 1);
    CHECK(s.totalFrames() == 4u);
    CHECK(s.dupFrames() == 0u);
}

TEST_CASE("RecordScheduler counts dups when the client falls behind (tick jumps)", "[record-scheduler]") {
    RecordScheduler s(30); // boundary every 2 ticks
    CHECK(s.boundariesDue(0) == 1);
    // The sim advanced 8 ticks in one render iteration: boundaries at 2,4,6,8 are all due -> 4 frames,
    // one fresh + three dups.
    CHECK(s.boundariesDue(8) == 4);
    CHECK(s.totalFrames() == 5u);
    CHECK(s.dupFrames() == 3u);
}

TEST_CASE("RecordScheduler emits nothing when no new boundary is reached", "[record-scheduler]") {
    RecordScheduler s(30);
    CHECK(s.boundariesDue(50) == 1); // anchor + first frame
    CHECK(s.boundariesDue(50) == 0); // same tick — nothing due
    CHECK(s.boundariesDue(51) == 0); // still inside the first boundary window (next is 52)
    CHECK(s.boundariesDue(52) == 1); // reached the next boundary
    CHECK(s.totalFrames() == 2u);
    CHECK(s.dupFrames() == 0u);
}
