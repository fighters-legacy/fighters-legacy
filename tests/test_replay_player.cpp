// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReplayPlayer (#41) — playback timing, scrubbing, and the delta cache that makes a seek land on a
// complete world.
//
// The property under test throughout: after ANY operation the player's published snapshot describes
// the world at `currentTick` completely. That is not free — records after a keyframe are deltas, so
// a seek that merely jumped the file cursor would paint entities into stale positions and look
// plausible while being wrong. Every seek case below checks the WORLD, not just the tick number.

#include "ReplayPlayer.h"
#include "flight/Geodetic.h"   // kEarthRadiusM
#include "net/SnapshotCodec.h" // QuantEntity + the record encoders the writer stitches
#include "replay/ReplayWriter.h"

#include "temp_path.h"
#include <catch2/catch_approx.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace fl;

namespace {

namespace fs = std::filesystem;

// A recording where entity 0's X position IS the tick index, so any frame can be checked against
// the tick it claims to be showing -- which is exactly what a wrong seek gets wrong.
fs::path writeRamp(const fs::path& dir, uint64_t ticks, uint32_t keyframeEvery, uint32_t tickRateHz = 60) {
    ReplayHeader h;
    h.engineVersion = "test";
    h.tickRateHz = tickRateHz;
    h.planetRadiusM = kEarthRadiusM;
    h.keyframeIntervalTicks = keyframeEvery;

    ReplaySections sections;
    sections.entityTypes.push_back({0, "builtin:debug-entity", "Debug", 0, 0});
    sections.factions.push_back({0, "neutral", "Neutral"});
    sections.roster.push_back({7, "Maverick", 0, 0, false});

    ReplayWriter::Config cfg;
    cfg.dir = dir;
    cfg.baseName = "ramp";

    ReplayWriter w;
    REQUIRE(w.open(h, sections, cfg));
    for (uint64_t t = 0; t < ticks; ++t) {
        const bool key = (t % keyframeEvery) == 0;
        QuantEntity e;
        e.idx = 0;
        e.gen = 1;
        e.typeIndex = 0;
        e.isFull = key;
        e.pos[0] = static_cast<double>(t); // the ramp
        e.pos[1] = 1000.0;
        e.quat[3] = 1.f;

        ReplayTick tick;
        tick.tickIndex = t;
        tick.flags = key ? kReplayTickKeyframe : uint16_t{0};
        double origin[3];
        originForPos(e.pos, origin);
        tick.origins = {origin[0], origin[1], origin[2]};
        std::vector<uint8_t> blob;
        encodeStandaloneRecord(blob, e, origin, /*sendGen=*/key);
        appendStitchedRecord(tick.records, 0, blob);
        tick.recordCount = 1;

        if (t == 3) {
            MatchEvent join;
            join.tick = t;
            join.type = MatchEventType::Join;
            join.actor = 9;
            join.text = "Iceman"; // a mid-recording joiner: not in the roster section
            tick.events.push_back(join);
        }
        REQUIRE(w.writeTick(tick));
    }
    REQUIRE(w.close());
    return dir / "ramp.flrep";
}

// The X position of entity 0 in the player's current world -- i.e. which tick is actually on screen.
double rampX(ReplayPlayer& p) {
    RenderSnapshot snap;
    REQUIRE(p.present(snap));
    REQUIRE(snap.entries.size() == 1);
    return snap.entries[0].position.x;
}

} // namespace

TEST_CASE("ReplayPlayer opens a recording and shows its first tick", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_open"};
    const fs::path p = writeRamp(tmp.path(), 60, 10);

    ReplayPlayer player;
    REQUIRE(player.open(p));
    CHECK(player.isOpen());
    CHECK(player.currentTick() == 0);
    CHECK(player.header().tickRateHz == 60);
    CHECK(player.durationSeconds() == Catch::Approx(59.0 / 60.0));
    // The world is visible before anything is played: a paused replay is not a black screen.
    CHECK(rampX(player) == Catch::Approx(0.0).margin(kPosStepM));
    // The roster section is seeded at open.
    REQUIRE(player.roster().count(7) == 1);
    CHECK(player.roster().at(7) == "Maverick");
}

TEST_CASE("ReplayPlayer refuses a file that is not a replay", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_refuse"};
    ReplayPlayer player;
    CHECK_FALSE(player.open(tmp.path() / "nothing.flrep"));
    CHECK_FALSE(player.lastError().empty());
    CHECK_FALSE(player.isOpen());
}

TEST_CASE("ReplayPlayer advances at the recording's own tick rate", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_rate"};
    // 30 Hz: the point of storing tickRateHz is that playback does not assume 60.
    const fs::path p = writeRamp(tmp.path(), 120, 10, /*tickRateHz=*/30);

    ReplayPlayer player;
    REQUIRE(player.open(p));
    RenderSnapshot snap;
    REQUIRE(player.update(0.0, snap)); // the first update publishes the opening frame

    // One second of wall time at 1x on a 30 Hz recording = 30 ticks, not 60.
    REQUIRE(player.update(1.0, snap));
    CHECK(player.currentTick() == 30);
    CHECK(snap.entries[0].position.x == Catch::Approx(30.0).margin(kPosStepM));
}

TEST_CASE("ReplayPlayer speed control scales advance, and pause freezes it", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_speed"};
    const fs::path p = writeRamp(tmp.path(), 600, 30);

    ReplayPlayer player;
    REQUIRE(player.open(p));
    RenderSnapshot snap;
    REQUIRE(player.update(0.0, snap));

    // Stepped in real frame-sized slices (1/60 s), because update() deliberately caps how far ONE
    // frame may advance -- see the catch-up case below.
    auto runOneSecond = [&](fl::TimeRate rate) {
        player.setRate(rate);
        for (int i = 0; i < 60; ++i)
            player.update(1.0 / 60.0, snap);
    };

    runOneSecond(TimeRate::Half);
    CHECK(player.currentTick() == 30); // 60 Hz * 1 s * 0.5

    runOneSecond(TimeRate::Double);
    CHECK(player.currentTick() == 150); // +120

    runOneSecond(TimeRate::Quarter);
    CHECK(player.currentTick() == 165); // +15

    // Paused: update() reports nothing new, and the tick does not move. This is what makes a photo
    // mode frame perfectly still rather than nearly still.
    player.setRate(TimeRate::Paused);
    const uint64_t frozen = player.currentTick();
    CHECK_FALSE(player.update(1.0, snap));
    CHECK(player.currentTick() == frozen);
    CHECK(player.paused());

    // togglePause returns to the rate it was playing at, not to 1x -- including when the pause came
    // from setRate() rather than from the key.
    player.togglePause();
    CHECK(player.rate() == TimeRate::Quarter);
}

TEST_CASE("ReplayPlayer caps how far one frame may advance", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_catchup"};
    const fs::path p = writeRamp(tmp.path(), 600, 30);
    ReplayPlayer player;
    REQUIRE(player.open(p));
    RenderSnapshot snap;
    REQUIRE(player.update(0.0, snap));

    // A frame that took a whole second (a window drag, a stalled disk) must not fast-forward the
    // recording by a whole second of playback -- the same spiral guard GameLoop applies to the sim.
    REQUIRE(player.update(1.0, snap));
    CHECK(player.currentTick() == 32);
}

TEST_CASE("ReplayPlayer sub-tick frames do not advance", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_subtick"};
    const fs::path p = writeRamp(tmp.path(), 60, 10);
    ReplayPlayer player;
    REQUIRE(player.open(p));
    RenderSnapshot snap;
    REQUIRE(player.update(0.0, snap));

    // A 1 ms frame is a fraction of a 60 Hz tick; the fraction must accumulate rather than round up
    // to a tick per frame (which would play the replay at frame rate instead of at 1x).
    for (int i = 0; i < 5; ++i)
        CHECK_FALSE(player.update(0.001, snap));
    CHECK(player.currentTick() == 0);

    // Accumulated: 5 ms + 12 ms = 17 ms > 1/60 s.
    CHECK(player.update(0.012, snap));
    CHECK(player.currentTick() == 1);
}

TEST_CASE("ReplayPlayer seeks land on a complete world, not a stale one", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_seek"};
    const fs::path p = writeRamp(tmp.path(), 200, 20);

    ReplayPlayer player;
    REQUIRE(player.open(p));

    SECTION("exact keyframe") {
        REQUIRE(player.seekToTick(60));
        CHECK(player.currentTick() == 60);
        CHECK(rampX(player) == Catch::Approx(60.0).margin(kPosStepM));
    }
    SECTION("between keyframes rolls forward from the one before") {
        REQUIRE(player.seekToTick(75));
        CHECK(player.currentTick() == 75);
        // The value that proves the roll-forward happened: a bare keyframe jump would show 60.
        CHECK(rampX(player) == Catch::Approx(75.0).margin(kPosStepM));
    }
    SECTION("backwards, the direction a simulation never runs") {
        REQUIRE(player.seekToTick(150));
        REQUIRE(player.seekToTick(25));
        CHECK(player.currentTick() == 25);
        CHECK(rampX(player) == Catch::Approx(25.0).margin(kPosStepM));
    }
    SECTION("to the very start") {
        REQUIRE(player.seekToTick(100));
        REQUIRE(player.seekToTick(0));
        CHECK(player.currentTick() == 0);
        CHECK(rampX(player) == Catch::Approx(0.0).margin(kPosStepM));
    }
    SECTION("past the end clamps to the last tick") {
        REQUIRE(player.seekToTick(99999));
        CHECK(player.currentTick() == 199);
        CHECK(rampX(player) == Catch::Approx(199.0).margin(kPosStepM));
    }
    SECTION("by seconds, forward and back") {
        REQUIRE(player.seekToTick(120));
        REQUIRE(player.seekBySeconds(-1.0)); // 60 ticks at 60 Hz
        CHECK(player.currentTick() == 60);
        REQUIRE(player.seekBySeconds(0.5));
        CHECK(player.currentTick() == 90);
        // Before the beginning clamps rather than underflowing the unsigned tick.
        REQUIRE(player.seekBySeconds(-1000.0));
        CHECK(player.currentTick() == 0);
    }
    SECTION("by fraction, the scrub bar's vocabulary") {
        REQUIRE(player.seekToFraction(0.5));
        CHECK(player.currentTick() == 100); // (199-0) * 0.5, rounded
        REQUIRE(player.seekToFraction(0.0));
        CHECK(player.currentTick() == 0);
        REQUIRE(player.seekToFraction(1.0));
        CHECK(player.currentTick() == 199);
        // Out-of-range fractions clamp instead of seeking off the end of the file.
        REQUIRE(player.seekToFraction(-3.0));
        CHECK(player.currentTick() == 0);
        REQUIRE(player.seekToFraction(9.0));
        CHECK(player.currentTick() == 199);
    }
}

TEST_CASE("ReplayPlayer progress and elapsed track the cursor", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_progress"};
    const fs::path p = writeRamp(tmp.path(), 121, 30);
    ReplayPlayer player;
    REQUIRE(player.open(p));

    CHECK(player.progress() == Catch::Approx(0.0));
    REQUIRE(player.seekToFraction(1.0));
    CHECK(player.progress() == Catch::Approx(1.0));
    CHECK(player.elapsedSeconds() == Catch::Approx(2.0)); // 120 ticks at 60 Hz
}

TEST_CASE("ReplayPlayer stops at the end and stays there", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_end"};
    const fs::path p = writeRamp(tmp.path(), 30, 10);
    ReplayPlayer player;
    REQUIRE(player.open(p));
    RenderSnapshot snap;
    REQUIRE(player.update(0.0, snap));

    // Far more wall time than the recording holds.
    player.update(10.0, snap);
    CHECK(player.atEnd());
    CHECK(player.currentTick() == 29);
    // Further updates do nothing rather than looping or running off the end.
    CHECK_FALSE(player.update(1.0, snap));
    CHECK(player.currentTick() == 29);

    // ...and a seek revives it: reaching the end is not a terminal state for the file.
    REQUIRE(player.seekToTick(5));
    CHECK_FALSE(player.atEnd());
    CHECK(player.currentTick() == 5);
}

TEST_CASE("ReplayPlayer picks up a callsign from a mid-recording join", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_roster"};
    const fs::path p = writeRamp(tmp.path(), 30, 10);
    ReplayPlayer player;
    REQUIRE(player.open(p));

    // The join is on tick 3, so it is not known at open...
    CHECK(player.roster().count(9) == 0);
    RenderSnapshot snap;
    REQUIRE(player.update(0.0, snap));
    player.update(0.2, snap); // past tick 3

    // ...and it is once playback reaches it. Without the callsign on the Join event, a participant
    // who joined after recording started would be "participant 9" forever.
    REQUIRE(player.roster().count(9) == 1);
    CHECK(player.roster().at(9) == "Iceman");
}

TEST_CASE("ReplayPlayer close leaves a reusable object", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_close"};
    const fs::path p = writeRamp(tmp.path(), 30, 10);
    ReplayPlayer player;
    REQUIRE(player.open(p));
    player.close();
    CHECK_FALSE(player.isOpen());

    RenderSnapshot snap;
    CHECK_FALSE(player.update(1.0, snap));
    CHECK_FALSE(player.present(snap));
    CHECK_FALSE(player.seekToTick(5));

    REQUIRE(player.open(p)); // reopens cleanly
    CHECK(player.currentTick() == 0);
}

TEST_CASE("ReplayPlayer keyframe ticks match the file's seek points", "[replay_player]") {
    fl::test::TempDirGuard tmp{"flrep_player_keys"};
    const fs::path p = writeRamp(tmp.path(), 100, 25);
    ReplayPlayer player;
    REQUIRE(player.open(p));

    const std::vector<uint64_t> keys = player.keyframeTicks();
    REQUIRE(keys.size() == 4);
    CHECK(keys[0] == 0);
    CHECK(keys[1] == 25);
    CHECK(keys[3] == 75);

    // The transport bar draws these as ticks, so they must be where a scrub actually lands.
    REQUIRE(player.seekToTick(30));
    CHECK(player.currentTick() == 30);
    CHECK(rampX(player) == Catch::Approx(30.0).margin(kPosStepM));
}
