// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `.flrep` section list, read from a hostile file (#1145).
//
// test_replay_format.cpp covers the header refusals and a truncated body. The SECTION LIST between
// them — the entity-type manifest, faction table and roster — is where a replay carries counts and
// lengths that a reader has to believe just long enough to check them. Every one of these is a
// number chosen by whoever wrote the file, and a replay is a file downloaded from a stranger.
//
// The failure that matters is not a crash. It is a count of four billion entity types reserved into
// a vector before anything notices the file is 300 bytes long. So each case here declares something
// the file cannot back and asserts the reader says no, by name, without having allocated for it.

#include <catch2/catch_test_macros.hpp>

#include "net/ByteOrder.h"
#include "replay/ReplayReader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fl;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("flrep_sections_" + tag);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A valid header, up to but not including the section list.
std::vector<uint8_t> headerBytes() {
    std::vector<uint8_t> b;
    b.insert(b.end(), std::begin(kReplayMagic), std::end(kReplayMagic));
    detail::putU16LE(b, kReplayFormatMajor);
    detail::putU16LE(b, kReplayFormatMinor);
    detail::putStringLE(b, "0.3.12");
    detail::putU32LE(b, 60); // tick rate
    detail::putF64LE(b, 6371000.0);
    detail::putU64LE(b, 0);
    detail::putStringLE(b, ""); // mission id
    detail::putU32LE(b, 0);     // session flags
    detail::putU32LE(b, 300);   // keyframe interval
    return b;
}

struct Writer {
    TempDir dir;
    int n{0};
    explicit Writer(const std::string& tag) : dir(tag) {}

    fs::path write(const std::vector<uint8_t>& b) {
        const fs::path p = dir.path / ("case" + std::to_string(n++) + ".flrep");
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return p;
    }
};

// A section whose payload is `payload`, followed by the terminator.
void appendSection(std::vector<uint8_t>& b, ReplaySectionId id, const std::vector<uint8_t>& payload) {
    detail::putU16LE(b, static_cast<uint16_t>(id));
    detail::putU32LE(b, static_cast<uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
}

// A trailer for a session that recorded no ticks: a zero-entry index, the first/last tick pair, and
// the 8-byte offset back to the trailer's own start. A file without one is not corrupt — the reader
// rebuilds by scanning — but the tests below are about the SECTIONS, so they hand it a good one.
void appendEmptyTrailer(std::vector<uint8_t>& b) {
    const uint64_t trailerOffset = b.size();
    detail::putU32LE(b, 0); // index entries
    detail::putU64LE(b, 0); // first tick
    detail::putU64LE(b, 0); // last tick
    detail::putU64LE(b, trailerOffset);
}

} // namespace

// ---------------------------------------------------------------------------
// The section list itself
// ---------------------------------------------------------------------------

TEST_CASE("flrep: a section list that stops mid-header is refused (#1145)", "[replay]") {
    // An interrupted write during the header, before any tick was recorded. There is nothing to
    // salvage and no terminator to find.
    Writer w("truncated");
    std::vector<uint8_t> b = headerBytes(); // no section list at all, not even a terminator
    ReplayReader r;
    CHECK_FALSE(r.open(w.write(b)));
    CHECK(r.lastError().find("section list") != std::string::npos);

    // A section id with no length behind it.
    std::vector<uint8_t> partial = headerBytes();
    detail::putU16LE(partial, static_cast<uint16_t>(ReplaySectionId::FactionTable));
    ReplayReader r2;
    CHECK_FALSE(r2.open(w.write(partial)));
}

TEST_CASE("flrep: a section declaring an impossible length is refused (#1145)", "[replay]") {
    // Four gigabytes of faction table in a 40-byte file. The length is checked against the cap
    // BEFORE it is used to advance the cursor, so nothing is reserved on the strength of it.
    Writer w("bigsection");
    std::vector<uint8_t> b = headerBytes();
    detail::putU16LE(b, static_cast<uint16_t>(ReplaySectionId::FactionTable));
    detail::putU32LE(b, 0xFFFFFFFFu);

    ReplayReader r;
    CHECK_FALSE(r.open(w.write(b)));
    CHECK(r.lastError().find("impossible length") != std::string::npos);
}

TEST_CASE("flrep: a section running past the end of the file is refused (#1145)", "[replay]") {
    // A plausible length — under the cap — that the file simply does not contain. This is the case a
    // cap alone would miss.
    Writer w("overrun");
    std::vector<uint8_t> b = headerBytes();
    detail::putU16LE(b, static_cast<uint16_t>(ReplaySectionId::Roster));
    detail::putU32LE(b, 4096); // but only a few bytes follow
    detail::putU32LE(b, 1);

    ReplayReader r;
    CHECK_FALSE(r.open(w.write(b)));
    CHECK(r.lastError().find("past the end") != std::string::npos);
}

TEST_CASE("flrep: an unknown section id is skipped by its length (#1145)", "[replay]") {
    // This is the forward-compatibility contract: a newer writer adds a section, an older reader
    // steps over it using the length field and keeps reading. Without it, every added section would
    // break every existing build.
    Writer w("unknown");
    std::vector<uint8_t> b = headerBytes();
    appendSection(b, static_cast<ReplaySectionId>(0xBEEF), {1, 2, 3, 4, 5, 6, 7, 8});

    // A faction table AFTER the unknown section: reaching it proves the skip landed exactly right.
    std::vector<uint8_t> factions;
    detail::putU32LE(factions, 1);
    detail::putU16LE(factions, 7);
    detail::putStringLE(factions, "blue");
    detail::putStringLE(factions, "Blue Force");
    appendSection(b, ReplaySectionId::FactionTable, factions);
    detail::putU16LE(b, 0); // terminator
    appendEmptyTrailer(b);

    ReplayReader r;
    REQUIRE(r.open(w.write(b)));
    REQUIRE(r.sections().factions.size() == 1u);
    CHECK(r.sections().factions[0].factionIndex == 7);
    CHECK(r.sections().factions[0].id == "blue");
}

// ---------------------------------------------------------------------------
// Declared counts
// ---------------------------------------------------------------------------

TEST_CASE("flrep: an impossible entity-type count is refused before it is reserved (#1145)", "[replay]") {
    Writer w("types");
    std::vector<uint8_t> payload;
    detail::putU32LE(payload, 0xFFFFFFFFu); // more entity types than the format allows

    std::vector<uint8_t> b = headerBytes();
    appendSection(b, ReplaySectionId::EntityTypeManifest, payload);
    detail::putU16LE(b, 0);

    ReplayReader r;
    CHECK_FALSE(r.open(w.write(b)));
    CHECK(r.lastError().find("entity-type manifest") != std::string::npos);
}

TEST_CASE("flrep: an impossible faction count is refused (#1145)", "[replay]") {
    Writer w("factions");
    std::vector<uint8_t> payload;
    detail::putU32LE(payload, 0xFFFFFFFFu);

    std::vector<uint8_t> b = headerBytes();
    appendSection(b, ReplaySectionId::FactionTable, payload);
    detail::putU16LE(b, 0);

    ReplayReader r;
    CHECK_FALSE(r.open(w.write(b)));
    CHECK(r.lastError().find("faction table") != std::string::npos);
}

TEST_CASE("flrep: an impossible roster count is refused (#1145)", "[replay]") {
    Writer w("roster");
    std::vector<uint8_t> payload;
    detail::putU32LE(payload, 0xFFFFFFFFu);

    std::vector<uint8_t> b = headerBytes();
    appendSection(b, ReplaySectionId::Roster, payload);
    detail::putU16LE(b, 0);

    ReplayReader r;
    CHECK_FALSE(r.open(w.write(b)));
    CHECK(r.lastError().find("roster") != std::string::npos);
}

TEST_CASE("flrep: a count larger than the entries that follow is a truncated section (#1145)", "[replay]") {
    // The count is plausible and under the cap, but the payload runs out partway through. The reader
    // refuses the whole file rather than keeping the entries that happened to parse.
    //
    // That is the stricter of the two options and it is the right one here: the faction table maps
    // indices to sides, so a partial one renders a replay in which some aircraft belong to nobody.
    // A replay that is visibly broken is better than one that is quietly wrong.
    Writer w("shortcount");
    std::vector<uint8_t> payload;
    detail::putU32LE(payload, 4); // claims four
    detail::putU16LE(payload, 1);
    detail::putStringLE(payload, "red");
    detail::putStringLE(payload, "Red Force"); // supplies one

    std::vector<uint8_t> b = headerBytes();
    appendSection(b, ReplaySectionId::FactionTable, payload);
    detail::putU16LE(b, 0);
    appendEmptyTrailer(b);

    ReplayReader r;
    CHECK_FALSE(r.open(w.write(b)));
    CHECK(r.lastError().find("truncated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// A file with sections and no ticks
// ---------------------------------------------------------------------------

TEST_CASE("flrep: sections parse into the reader's tables (#1145)", "[replay]") {
    // The positive control for everything above: the same builder, well formed, must produce exactly
    // what went in. Otherwise a refusal test proves only that the builder is broken.
    Writer w("good");
    std::vector<uint8_t> types;
    detail::putU32LE(types, 1);
    detail::putU32LE(types, 3); // typeIndex
    detail::putStringLE(types, "fl-base:f5e");
    detail::putStringLE(types, "F-5E");
    types.push_back(1); // category
    types.push_back(0); // projectileKind

    std::vector<uint8_t> roster;
    detail::putU32LE(roster, 1);
    detail::putU32LE(roster, 42); // participantId
    detail::putStringLE(roster, "Viper 1");
    detail::putU16LE(roster, 1); // factionIndex
    roster.push_back(0);         // role
    roster.push_back(1);         // isBot

    std::vector<uint8_t> b = headerBytes();
    appendSection(b, ReplaySectionId::EntityTypeManifest, types);
    appendSection(b, ReplaySectionId::Roster, roster);
    detail::putU16LE(b, 0);
    appendEmptyTrailer(b);

    ReplayReader r;
    REQUIRE(r.open(w.write(b)));

    REQUIRE(r.sections().entityTypes.size() == 1u);
    CHECK(r.sections().entityTypes[0].typeIndex == 3u);
    CHECK(r.sections().entityTypes[0].id == "fl-base:f5e");
    CHECK(r.sections().entityTypes[0].name == "F-5E");

    REQUIRE(r.sections().roster.size() == 1u);
    CHECK(r.sections().roster[0].participantId == 42u);
    CHECK(r.sections().roster[0].callsign == "Viper 1");
    CHECK(r.sections().roster[0].isBot);

    // No ticks at all is a valid, empty replay — not a corrupt one.
    CHECK(r.durationSeconds() == 0.0);
    CHECK(r.header().tickRateHz == 60u);
}

// ---------------------------------------------------------------------------
// The trailer index
// ---------------------------------------------------------------------------

namespace {

// A file with valid sections, then whatever trailer bytes the caller supplies.
std::vector<uint8_t> fileWithTrailer(const std::vector<uint8_t>& trailer) {
    std::vector<uint8_t> b = headerBytes();
    detail::putU16LE(b, 0); // an empty section list
    b.insert(b.end(), trailer.begin(), trailer.end());
    return b;
}

} // namespace

TEST_CASE("flrep: a trailer pointing outside the file is not followed (#1145)", "[replay]") {
    // The last 8 bytes say where the index starts, and a hostile file can put anything there. A
    // trailer beginning before the frame stream or past the end of the file is rejected, the reader
    // falls back to scanning for chunks, and — since these files have no chunks — the open fails
    // with that diagnosis. The point is WHICH failure: following the offset would have seeked into
    // the header or off the end of the file instead.
    Writer w("badoffset");

    SECTION("before the body") {
        std::vector<uint8_t> t;
        detail::putU64LE(t, 0); // offset 0 is inside the header
        ReplayReader r;
        CHECK_FALSE(r.open(w.write(fileWithTrailer(t))));
        CHECK(r.lastError().find("no readable frame data") != std::string::npos);
    }
    SECTION("past the end") {
        std::vector<uint8_t> t;
        detail::putU64LE(t, 0xFFFFFFFFFFFFFF00ull);
        ReplayReader r;
        CHECK_FALSE(r.open(w.write(fileWithTrailer(t))));
        CHECK(r.lastError().find("no readable frame data") != std::string::npos);
    }
}

TEST_CASE("flrep: a trailer whose entry count does not fit is rejected (#1145)", "[replay]") {
    // Sixteen bytes per entry, and they have to fit in what the trailer actually occupies. Believing
    // the count instead would read the index off the end of the buffer.
    Writer w("badcount");
    std::vector<uint8_t> t;
    const uint64_t trailerOffset = headerBytes().size() + 2;
    detail::putU32LE(t, 100000); // claims a hundred thousand index entries
    detail::putU64LE(t, 0);
    detail::putU64LE(t, 0);
    detail::putU64LE(t, trailerOffset);

    ReplayReader r;
    CHECK_FALSE(r.open(w.write(fileWithTrailer(t)))); // rejected; the scan takes over and finds nothing
    CHECK(r.lastError().find("no readable frame data") != std::string::npos);
}

TEST_CASE("flrep: a trailer whose offsets do not increase is rejected (#1145)", "[replay]") {
    // Strictly increasing offsets are what stop a hostile trailer from making a seek loop: an index
    // that points backwards would let a crafted file drive the player round the same chunk forever.
    Writer w("loop");
    const uint64_t bodyOffset = headerBytes().size() + 2;
    const uint64_t trailerOffset = bodyOffset + 64; // pretend 64 bytes of frame stream

    auto build = [&](uint64_t offA, uint64_t offB, uint64_t tickA, uint64_t tickB) {
        std::vector<uint8_t> b = headerBytes();
        detail::putU16LE(b, 0);
        b.resize(static_cast<std::size_t>(trailerOffset), 0); // filler standing in for chunks
        detail::putU32LE(b, 2);
        detail::putU64LE(b, tickA);
        detail::putU64LE(b, offA);
        detail::putU64LE(b, tickB);
        detail::putU64LE(b, offB);
        detail::putU64LE(b, tickA);
        detail::putU64LE(b, tickB);
        detail::putU64LE(b, trailerOffset);
        return b;
    };

    SECTION("offsets go backwards") {
        ReplayReader r;
        CHECK_FALSE(r.open(w.write(build(bodyOffset + 32, bodyOffset, 0, 10))));
    }
    SECTION("ticks go backwards") {
        ReplayReader r;
        CHECK_FALSE(r.open(w.write(build(bodyOffset, bodyOffset + 32, 10, 5))));
    }
    SECTION("an entry points past the trailer") {
        ReplayReader r;
        CHECK_FALSE(r.open(w.write(build(bodyOffset, trailerOffset + 8, 0, 10))));
    }
}

TEST_CASE("flrep: durationSeconds is zero for a replay with no extent (#1145)", "[replay]") {
    // Reported in the UI. A negative or nonsense duration on an empty or damaged file would show as
    // a scrub bar the player can drag into nothing.
    Writer w("duration");
    std::vector<uint8_t> t;
    detail::putU32LE(t, 0);
    detail::putU64LE(t, 0);
    detail::putU64LE(t, 0);
    detail::putU64LE(t, headerBytes().size() + 2);

    ReplayReader r;
    REQUIRE(r.open(w.write(fileWithTrailer(t))));
    CHECK(r.durationSeconds() == 0.0);

    // Re-opening a bad path after a good one leaves the reader closed, not holding the old file.
    CHECK_FALSE(r.open(w.dir.path / "not_here.flrep"));
    ReplayTick tick;
    CHECK_FALSE(r.readNextTick(tick));
    CHECK(r.durationSeconds() == 0.0);
}
