// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReplaySelectScreen (#41) — the recorded-match browser.
//
// The behaviour worth pinning is the unhappy one: a `.flrep` this build cannot read is LISTED, greyed
// out, with the reader's own refusal attached, and confirming it refuses instead of entering a
// session that cannot start. Omitting unreadable files would leave a player looking at a directory
// full of recordings and a menu insisting there are none.

#include "ReplaySelectScreen.h"
#include "flight/Geodetic.h"   // kEarthRadiusM
#include "net/SnapshotCodec.h" // QuantEntity + the record encoders the writer stitches
#include "replay/ReplayWriter.h"

#include "mock_hal.h"

#include "temp_path.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace fl;

// These suites drive the screen a fixed step at a time; nothing here is rate-dependent (#1241).
constexpr float kTestFrameDtS = 1.f / 60.f;

namespace {

namespace fs = std::filesystem;

void writeReplay(const fs::path& dir, const std::string& name, const std::string& mission, uint64_t ticks = 10) {
    ReplayHeader h;
    h.engineVersion = "0.3.12";
    h.tickRateHz = 60;
    h.planetRadiusM = kEarthRadiusM;
    h.startUnixSeconds = 1769500000ull;
    h.missionId = mission;

    ReplaySections sections;
    sections.entityTypes.push_back({0, "builtin:debug-entity", "Debug", 0, 0});

    ReplayWriter::Config cfg;
    cfg.dir = dir;
    cfg.baseName = name;

    ReplayWriter w;
    REQUIRE(w.open(h, sections, cfg));
    for (uint64_t t = 0; t < ticks; ++t) {
        QuantEntity e;
        e.idx = 0;
        e.gen = 1;
        e.isFull = true;
        e.pos[1] = 500.0;
        e.quat[3] = 1.f;
        ReplayTick tick;
        tick.tickIndex = t;
        tick.flags = kReplayTickKeyframe;
        double origin[3];
        originForPos(e.pos, origin);
        tick.origins = {origin[0], origin[1], origin[2]};
        std::vector<uint8_t> blob;
        encodeStandaloneRecord(blob, e, origin, true);
        appendStitchedRecord(tick.records, 0, blob);
        tick.recordCount = 1;
        REQUIRE(w.writeTick(tick));
    }
    REQUIRE(w.close());
}

} // namespace

TEST_CASE("ReplaySelectScreen scan of a missing directory is empty, not an error", "[replay_select]") {
    fl::test::TempDirGuard tmp{"flrep_select_missing"};
    // A player who has never recorded anything has no replays directory. That is not a failure.
    const auto entries = ReplaySelectScreen::scan(tmp.path() / "does-not-exist");
    CHECK(entries.empty());
}

TEST_CASE("ReplaySelectScreen lists readable replays with their header details", "[replay_select]") {
    fl::test::TempDirGuard tmp{"flrep_select_list"};
    writeReplay(tmp.path(), "one", "ci_smoke", 121);
    writeReplay(tmp.path(), "two", "", 61);

    const auto entries = ReplaySelectScreen::scan(tmp.path());
    REQUIRE(entries.size() == 2);
    for (const auto& e : entries) {
        CHECK(e.playable);
        CHECK_FALSE(e.label.empty());
        CHECK(e.detail.find("0.3.12") != std::string::npos); // the recorder's version
    }
    const bool sawMission = entries[0].label.find("ci_smoke") != std::string::npos ||
                            entries[1].label.find("ci_smoke") != std::string::npos;
    const bool sawFreeFlight = entries[0].label.find("free flight") != std::string::npos ||
                               entries[1].label.find("free flight") != std::string::npos;
    CHECK(sawMission);
    CHECK(sawFreeFlight); // an empty missionId reads as "free flight", not as a blank column
}

TEST_CASE("ReplaySelectScreen lists an unreadable replay rather than hiding it", "[replay_select]") {
    fl::test::TempDirGuard tmp{"flrep_select_broken"};
    writeReplay(tmp.path(), "good", "ci_smoke");
    {
        std::ofstream out(tmp.path() / "broken.flrep", std::ios::binary | std::ios::trunc);
        out << "this is not a replay";
    }

    const auto entries = ReplaySelectScreen::scan(tmp.path());
    REQUIRE(entries.size() == 2);

    int playable = 0;
    int refused = 0;
    for (const auto& e : entries) {
        if (e.playable) {
            ++playable;
        } else {
            ++refused;
            CHECK(e.label == "broken.flrep"); // named by file, since its header said nothing
            CHECK_FALSE(e.detail.empty());    // and carrying the reader's reason
        }
    }
    CHECK(playable == 1);
    CHECK(refused == 1);
}

TEST_CASE("ReplaySelectScreen confirm enters a session for a readable file", "[replay_select]") {
    fl::test::TempDirGuard tmp{"flrep_select_confirm"};
    writeReplay(tmp.path(), "one", "ci_smoke");

    ReplaySelectScreen screen(ReplaySelectScreen::scan(tmp.path()));
    REQUIRE(screen.entryCount() == 1);

    MockInput input;
    MockWindow window;
    input.justPressed = {Key::Enter};
    const Screen next = screen.update(input, window, kTestFrameDtS);
    CHECK(next == Screen::Loading);
    CHECK(screen.selectedReplay() == tmp.path() / "one.flrep");
    CHECK(screen.statusText().empty());
}

TEST_CASE("ReplaySelectScreen confirm on an unreadable file refuses and stays put", "[replay_select]") {
    fl::test::TempDirGuard tmp{"flrep_select_refuse"};
    {
        std::ofstream out(tmp.path() / "broken.flrep", std::ios::binary | std::ios::trunc);
        out << "nope";
    }

    ReplaySelectScreen screen(ReplaySelectScreen::scan(tmp.path()));
    REQUIRE(screen.entryCount() == 1);

    MockInput input;
    MockWindow window;
    input.justPressed = {Key::Enter};
    const Screen next = screen.update(input, window, kTestFrameDtS);
    // Refusing here is the whole point: entering Loading would start a session that cannot begin.
    CHECK(next == Screen::ReplaySelect);
    CHECK(screen.selectedReplay().empty());
    CHECK_FALSE(screen.statusText().empty());
}

TEST_CASE("ReplaySelectScreen escape returns to the menu and an empty list is harmless", "[replay_select]") {
    fl::test::TempDirGuard tmp{"flrep_select_empty"};
    ReplaySelectScreen screen(ReplaySelectScreen::scan(tmp.path()));
    CHECK(screen.entryCount() == 0);

    MockInput input;
    MockWindow window;

    // Confirm on an empty list must not index off the end.
    input.justPressed = {Key::Enter};
    CHECK(screen.update(input, window, kTestFrameDtS) == Screen::ReplaySelect);
    CHECK(screen.selectedReplay().empty());
    CHECK_FALSE(screen.buildElements().empty()); // still draws a title and "no recordings yet"

    input.justPressed = {Key::Escape};
    CHECK(screen.update(input, window, kTestFrameDtS) == Screen::MainMenu);
}

TEST_CASE("ReplaySelectScreen navigation moves and clamps the selection", "[replay_select]") {
    fl::test::TempDirGuard tmp{"flrep_select_nav"};
    for (int i = 0; i < 3; ++i)
        writeReplay(tmp.path(), "r" + std::to_string(i), "m" + std::to_string(i));

    ReplaySelectScreen screen(ReplaySelectScreen::scan(tmp.path()));
    REQUIRE(screen.entryCount() == 3);

    MockInput input;
    MockWindow window;

    input.justPressed = {Key::ArrowUp}; // already at the top: clamps rather than wrapping
    screen.update(input, window, kTestFrameDtS);
    CHECK(screen.selectedIndex() == 0);

    input.justPressed = {Key::ArrowDown};
    screen.update(input, window, kTestFrameDtS);
    CHECK(screen.selectedIndex() == 1);
    screen.update(input, window, kTestFrameDtS);
    CHECK(screen.selectedIndex() == 2);
    screen.update(input, window, kTestFrameDtS);
    CHECK(screen.selectedIndex() == 2); // clamps at the end

    CHECK_FALSE(screen.buildElements().empty());
}
