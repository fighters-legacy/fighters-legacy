// SPDX-License-Identifier: GPL-3.0-or-later
//
// `.flrep` writer/reader (#643) — the format from docs/replay-format.md.
//
// Two things are being defended here. The first is ROUND-TRIP FIDELITY: what comes out of the file
// must be what went in, in the quantized domain the codec actually transmits, because a replay that
// is subtly wrong renders a plausible flight nobody notices is fiction. The second is HOSTILE INPUT:
// a replay is a file downloaded from a stranger, so every declared length, count and size is checked
// against reality before it is believed -- and these tests assert the refusals, not just the happy
// path (fuzz/fuzz_flrep.cpp covers the same reader with random bytes).

#include "net/BitStream.h"
#include "net/ByteOrder.h"
#include "net/ReplayStateHash.h"
#include "net/SnapshotCodec.h"
#include "replay/ReplayReader.h"
#include "replay/ReplayWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fl;

namespace {

namespace fs = std::filesystem;

// A temp directory that cleans itself up, so a failing assertion cannot leave a stale .flrep behind
// to be read by the next run.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("flrep_test_" + tag);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

QuantEntity makeEntity(uint32_t idx, double x, double y, double z, uint32_t typeIndex = 3) {
    QuantEntity e;
    e.idx = idx;
    e.gen = 1;
    e.typeIndex = typeIndex;
    e.factionIndex = static_cast<uint16_t>(idx % 3);
    e.pos[0] = x;
    e.pos[1] = y;
    e.pos[2] = z;
    e.vel[0] = 120.f;
    e.vel[1] = -1.5f;
    e.vel[2] = 3.25f;
    e.quat[0] = 0.f;
    e.quat[1] = 0.f;
    e.quat[2] = 0.f;
    e.quat[3] = 1.f;
    e.damageLevel = 1;
    e.engineFailFlags = 2;
    e.throttle = 80;
    e.fuelPct = 55;
    e.abEngaged = true;
    e.playerOwned = (idx == 0);
    return e;
}

// Build one tick the way WorldBroadcaster's replay tap does: encode each entity once, stitch the
// blobs with an origin-table index. Keeping the test on the SAME codec path is the point -- a test
// with its own encoder would prove the test round-trips, not the format.
ReplayTick makeTick(uint64_t tickIndex, const std::vector<QuantEntity>& ents, bool keyframe) {
    ReplayTick t;
    t.tickIndex = tickIndex;
    t.flags = keyframe ? kReplayTickKeyframe : uint16_t{0};

    auto originIndexOf = [&t](const double o[3]) -> uint32_t {
        for (std::size_t i = 0; i * 3 + 2 < t.origins.size(); ++i) {
            if (t.origins[i * 3] == o[0] && t.origins[i * 3 + 1] == o[1] && t.origins[i * 3 + 2] == o[2])
                return static_cast<uint32_t>(i);
        }
        t.origins.push_back(o[0]);
        t.origins.push_back(o[1]);
        t.origins.push_back(o[2]);
        return static_cast<uint32_t>(t.origins.size() / 3 - 1);
    };

    for (const QuantEntity& e : ents) {
        double origin[3];
        originForPos(e.pos, origin);
        QuantEntity q = e;
        q.isFull = keyframe;
        std::vector<uint8_t> blob;
        encodeStandaloneRecord(blob, q, origin, /*sendGen=*/keyframe);
        appendStitchedRecord(t.records, originIndexOf(origin), blob);
        ++t.recordCount;
    }
    return t;
}

// Decode a tick's record stream back into QuantEntities, filling delta records from a cache exactly
// as a player must (a delta carries no typeIndex, factionIndex or gen).
std::vector<QuantEntity> decodeTick(const ReplayTick& t, std::unordered_map<uint32_t, QuantEntity>& cache) {
    std::vector<QuantEntity> out;
    BitReader r(t.records.data(), t.records.size());
    const auto originCount = static_cast<uint32_t>(t.origins.size() / 3);
    for (uint16_t i = 0; i < t.recordCount; ++i) {
        QuantEntity e;
        bool genPresent = false;
        REQUIRE(decodeStandaloneRecord(r, e, t.origins.data(), originCount, genPresent));
        if (!e.isFull) {
            const auto it = cache.find(e.idx);
            REQUIRE(it != cache.end());
            e.typeIndex = it->second.typeIndex;
            e.factionIndex = it->second.factionIndex;
            if (!genPresent)
                e.gen = it->second.gen;
        }
        cache[e.idx] = e;
        out.push_back(e);
    }
    return out;
}

ReplayHeader makeHeader() {
    ReplayHeader h;
    h.engineVersion = "0.3.12";
    h.tickRateHz = 60;
    h.planetRadiusM = 3389500.0; // deliberately NOT Earth -- the header field exists for this reason
    h.startUnixSeconds = 1769500000ull;
    h.missionId = "ci_smoke";
    h.sessionFlags = kReplaySessionMission;
    h.keyframeIntervalTicks = 4;
    return h;
}

ReplaySections makeSections() {
    ReplaySections s;
    s.entityTypes.push_back({3, "fl-base:f15c", "F-15C Eagle", 1, 0});
    s.entityTypes.push_back({4, "fl-base:mig29", "MiG-29A Fulcrum", 1, 0});
    s.factions.push_back({0, "neutral", "Neutral"});
    s.factions.push_back({1, "blufor", "Blue Force"});
    s.roster.push_back({7, "Maverick", 1, 0, false});
    return s;
}

ReplayWriter::Config makeWriterConfig(const fs::path& dir, const std::string& base = "session") {
    ReplayWriter::Config c;
    c.dir = dir;
    c.baseName = base;
    return c;
}

} // namespace

TEST_CASE("flrep round-trips the header and sections", "[replay]") {
    TempDir tmp("header");
    const ReplayHeader header = makeHeader();
    const ReplaySections sections = makeSections();

    {
        ReplayWriter w;
        REQUIRE(w.open(header, sections, makeWriterConfig(tmp.path)));
        REQUIRE(w.writeTick(makeTick(0, {makeEntity(0, 10.0, 500.0, -20.0)}, true)));
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(tmp.path / "session.flrep"));
    CHECK(r.header().formatMajor == kReplayFormatMajor);
    CHECK(r.header().engineVersion == "0.3.12");
    CHECK(r.header().tickRateHz == 60);
    // The whole point of storing it: a non-Earth session must not come back as Earth.
    CHECK(r.header().planetRadiusM == 3389500.0);
    CHECK(r.header().startUnixSeconds == 1769500000ull);
    CHECK(r.header().missionId == "ci_smoke");
    CHECK((r.header().sessionFlags & kReplaySessionMission) != 0u);

    REQUIRE(r.sections().entityTypes.size() == 2);
    CHECK(r.sections().entityTypes[0].id == "fl-base:f15c");
    CHECK(r.sections().entityTypes[1].name == "MiG-29A Fulcrum");
    REQUIRE(r.sections().factions.size() == 2);
    CHECK(r.sections().factions[1].name == "Blue Force");
    REQUIRE(r.sections().roster.size() == 1);
    CHECK(r.sections().roster[0].callsign == "Maverick");
    CHECK_FALSE(r.indexRebuilt());
}

TEST_CASE("flrep round-trips entity records in the quantized domain", "[replay]") {
    TempDir tmp("records");
    std::vector<QuantEntity> ents{makeEntity(0, 1234.5, 3000.0, -987.25), makeEntity(5, -40000.0, 812.5, 65600.0),
                                  makeEntity(9, 0.0, 0.0, 0.0, 4)};

    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        REQUIRE(w.writeTick(makeTick(0, ents, true)));
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(tmp.path / "session.flrep"));
    ReplayTick t;
    REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 0);
    CHECK(t.keyframe());
    REQUIRE(t.recordCount == 3);

    std::unordered_map<uint32_t, QuantEntity> cache;
    const std::vector<QuantEntity> got = decodeTick(t, cache);
    REQUIRE(got.size() == 3);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].idx == ents[i].idx);
        CHECK(got[i].typeIndex == ents[i].typeIndex);
        CHECK(got[i].factionIndex == ents[i].factionIndex);
        CHECK(got[i].damageLevel == ents[i].damageLevel);
        CHECK(got[i].throttle == ents[i].throttle);
        CHECK(got[i].fuelPct == ents[i].fuelPct);
        CHECK(got[i].abEngaged == ents[i].abEngaged);
        CHECK(got[i].playerOwned == ents[i].playerOwned);
        // Position is lossy in FLOAT space by design (12.5 cm steps) and exact in the quantized
        // domain, which is the domain the state hash and the determinism gate compare in.
        CHECK(std::abs(got[i].pos[0] - ents[i].pos[0]) <= kPosStepM);
        CHECK(std::abs(got[i].pos[1] - ents[i].pos[1]) <= kPosStepM);
        CHECK(std::abs(got[i].pos[2] - ents[i].pos[2]) <= kPosStepM);
    }

    // The #644 property, asserted here at its source: the tick hash of what was written equals the
    // tick hash of what was read back.
    CHECK(hashTickState(0, ents.data(), ents.size()) == hashTickState(0, got.data(), got.size()));
    CHECK_FALSE(r.readNextTick(t)); // clean end of stream
    CHECK(r.lastError().empty());
}

TEST_CASE("flrep round-trips interleaved match events", "[replay]") {
    TempDir tmp("events");
    ReplayTick t = makeTick(12, {makeEntity(0, 5.0, 100.0, 5.0)}, true);

    MatchEvent kill;
    kill.seq = 41;
    kill.tick = 12;
    kill.type = MatchEventType::Kill;
    kill.subjectIdx = 5;
    kill.instigatorIdx = 0;
    kill.actor = 7;
    kill.target = 9;
    kill.weaponClass = 2;
    kill.value = 100;
    MatchEvent chat;
    chat.seq = 42;
    chat.tick = 12;
    chat.type = MatchEventType::Chat;
    chat.actor = 7;
    chat.channel = 1;
    chat.text = "splash one \"bandit\""; // quotes: the text is attacker-controlled, never escaped away
    t.events = {kill, chat};

    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        REQUIRE(w.writeTick(t));
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(tmp.path / "session.flrep"));
    ReplayTick got;
    REQUIRE(r.readNextTick(got));
    REQUIRE(got.events.size() == 2);
    CHECK(got.events[0].type == MatchEventType::Kill);
    CHECK(got.events[0].seq == 41);
    CHECK(got.events[0].subjectIdx == 5);
    CHECK(got.events[0].weaponClass == 2);
    CHECK(got.events[0].value == 100);
    CHECK(got.events[1].type == MatchEventType::Chat);
    CHECK(got.events[1].text == "splash one \"bandit\"");
}

TEST_CASE("flrep seeks to the keyframe at or before a tick", "[replay]") {
    TempDir tmp("seek");
    constexpr uint64_t kTicks = 20;
    constexpr uint64_t kInterval = 5;

    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        for (uint64_t i = 0; i < kTicks; ++i) {
            const bool key = (i % kInterval) == 0;
            REQUIRE(w.writeTick(makeTick(i, {makeEntity(0, 10.0 + static_cast<double>(i), 500.0, 0.0)}, key)));
        }
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(tmp.path / "session.flrep"));
    CHECK(r.index().size() == kTicks / kInterval);
    CHECK(r.firstTick() == 0);
    CHECK(r.lastTick() == kTicks - 1);
    CHECK(r.durationSeconds() > 0.0);

    ReplayTick t;
    // Mid-chunk target: the seek lands on the keyframe BEFORE it, and rolling forward reaches it.
    REQUIRE(r.seekToKeyframeAtOrBefore(13));
    REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 10);
    CHECK(t.keyframe());
    while (t.tickIndex < 13)
        REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 13);
    CHECK_FALSE(t.keyframe());

    // Exactly on a keyframe.
    REQUIRE(r.seekToKeyframeAtOrBefore(15));
    REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 15);

    // Before the beginning clamps to the first keyframe; past the end clamps to the last.
    REQUIRE(r.seekToKeyframeAtOrBefore(0));
    REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 0);
    REQUIRE(r.seekToKeyframeAtOrBefore(9999));
    REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 15);

    // Backwards seeking has no precedent in the engine (the sim only ever moves forward), so it is
    // asserted explicitly rather than assumed to fall out of the forward path.
    REQUIRE(r.seekToKeyframeAtOrBefore(3));
    REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 0);

    REQUIRE(r.rewind());
    REQUIRE(r.readNextTick(t));
    CHECK(t.tickIndex == 0);
}

TEST_CASE("flrep reads every tick back in order", "[replay]") {
    TempDir tmp("order");
    constexpr uint64_t kTicks = 37;
    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        for (uint64_t i = 0; i < kTicks; ++i)
            REQUIRE(w.writeTick(makeTick(i, {makeEntity(0, static_cast<double>(i), 500.0, 0.0)}, (i % 4) == 0)));
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(tmp.path / "session.flrep"));
    ReplayTick t;
    uint64_t expected = 0;
    while (r.readNextTick(t)) {
        CHECK(t.tickIndex == expected);
        ++expected;
    }
    CHECK(expected == kTicks);
    CHECK(r.lastError().empty());
}

TEST_CASE("flrep with no ticks is valid and empty, not corrupt", "[replay]") {
    TempDir tmp("empty");
    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(tmp.path / "session.flrep")); // a session that recorded nothing still opens
    CHECK(r.sections().entityTypes.size() == 2);
    ReplayTick t;
    CHECK_FALSE(r.readNextTick(t));
    CHECK(r.lastError().empty()); // empty is not an error
}

TEST_CASE("flrep survives a missing trailer by rebuilding the index", "[replay]") {
    TempDir tmp("trailer");
    constexpr uint64_t kTicks = 12;
    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        for (uint64_t i = 0; i < kTicks; ++i)
            REQUIRE(w.writeTick(makeTick(i, {makeEntity(0, static_cast<double>(i), 500.0, 0.0)}, (i % 4) == 0)));
        REQUIRE(w.close());
    }

    const fs::path good = tmp.path / "session.flrep";
    const fs::path cut = tmp.path / "cut.flrep";

    // Read the whole file, then keep only what was on disk before the trailer -- i.e. the file a
    // killed server leaves behind. The last chunk is intact; the trailer never happened.
    std::vector<char> bytes;
    {
        std::ifstream in(good, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    REQUIRE(bytes.size() > 16);
    uint64_t trailerOffset = 0;
    std::memcpy(&trailerOffset, bytes.data() + bytes.size() - 8, 8);
    REQUIRE(trailerOffset < bytes.size());
    {
        std::ofstream out(cut, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(trailerOffset));
    }

    ReplayReader r;
    REQUIRE(r.open(cut));
    CHECK(r.indexRebuilt()); // surfaced, because it means the recording was interrupted
    ReplayTick t;
    uint64_t count = 0;
    while (r.readNextTick(t))
        ++count;
    CHECK(count == kTicks); // nothing lost but the seek table
}

TEST_CASE("flrep truncated mid-chunk keeps the ticks before the cut", "[replay]") {
    TempDir tmp("truncated");
    constexpr uint64_t kTicks = 16;
    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        for (uint64_t i = 0; i < kTicks; ++i)
            REQUIRE(w.writeTick(makeTick(i, {makeEntity(0, static_cast<double>(i), 500.0, 0.0)}, (i % 4) == 0)));
        REQUIRE(w.close());
    }

    std::vector<char> bytes;
    {
        std::ifstream in(tmp.path / "session.flrep", std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    uint64_t trailerOffset = 0;
    std::memcpy(&trailerOffset, bytes.data() + bytes.size() - 8, 8);

    // Cut inside the LAST chunk: its header promises more bytes than exist.
    const fs::path cut = tmp.path / "mid.flrep";
    const auto keep = static_cast<std::streamsize>(trailerOffset - 8);
    {
        std::ofstream out(cut, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), keep);
    }

    ReplayReader r;
    REQUIRE(r.open(cut)); // opens: the earlier chunks are intact
    ReplayTick t;
    uint64_t count = 0;
    while (r.readNextTick(t))
        ++count;
    // Whole ticks only -- a partially-written chunk yields no partial tick.
    CHECK(count > 0);
    CHECK(count < kTicks);
}

TEST_CASE("flrep refuses a newer major and names both versions", "[replay]") {
    TempDir tmp("major");
    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        REQUIRE(w.writeTick(makeTick(0, {makeEntity(0, 0.0, 500.0, 0.0)}, true)));
        REQUIRE(w.close());
    }

    // Bump the major in place: byte 4 of the file.
    const fs::path bumped = tmp.path / "future.flrep";
    fs::copy_file(tmp.path / "session.flrep", bumped);
    {
        std::fstream f(bumped, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(4, std::ios::beg);
        const uint16_t major = kReplayFormatMajor + 3;
        f.write(reinterpret_cast<const char*>(&major), 2);
    }

    ReplayReader r;
    CHECK_FALSE(r.open(bumped));
    // A refusal nobody can misread beats a partial render nobody notices.
    CHECK(r.lastError().find(std::to_string(kReplayFormatMajor + 3)) != std::string::npos);
    CHECK(r.lastError().find(std::to_string(kReplayFormatMajor)) != std::string::npos);
}

TEST_CASE("flrep reads a newer minor with an unknown section", "[replay]") {
    TempDir tmp("minor");
    // Hand-build a file whose minor is ahead and which carries a section id this build has never
    // heard of. Skipping it by its declared length is what makes "additive is a minor bump" true.
    std::vector<uint8_t> f;
    f.insert(f.end(), std::begin(kReplayMagic), std::end(kReplayMagic));
    detail::putU16LE(f, kReplayFormatMajor);
    detail::putU16LE(f, kReplayFormatMinor + 7);
    detail::putStringLE(f, "9.9.9");
    detail::putU32LE(f, 60);
    detail::putF64LE(f, 6371000.0);
    detail::putU64LE(f, 1769500000ull);
    detail::putStringLE(f, "from_the_future");
    detail::putU32LE(f, 0);
    detail::putU32LE(f, 300);
    // An unknown section, then a known one after it -- so the test proves the reader RESUMED
    // correctly rather than merely not crashing.
    std::vector<uint8_t> unknown{1, 2, 3, 4, 5, 6, 7};
    detail::putU16LE(f, 0x00FFu);
    detail::putU32LE(f, static_cast<uint32_t>(unknown.size()));
    f.insert(f.end(), unknown.begin(), unknown.end());
    std::vector<uint8_t> factions;
    detail::putU32LE(factions, 1);
    detail::putU16LE(factions, 4);
    detail::putStringLE(factions, "opfor");
    detail::putStringLE(factions, "Red Force");
    detail::putU16LE(f, static_cast<uint16_t>(ReplaySectionId::FactionTable));
    detail::putU32LE(f, static_cast<uint32_t>(factions.size()));
    f.insert(f.end(), factions.begin(), factions.end());
    detail::putU16LE(f, 0u); // section terminator
    // No chunks, and a valid empty trailer.
    const uint64_t trailerOffset = f.size();
    detail::putU32LE(f, 0);
    detail::putU64LE(f, 0);
    detail::putU64LE(f, 0);
    detail::putU64LE(f, trailerOffset);

    const fs::path p = tmp.path / "minor.flrep";
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    }

    ReplayReader r;
    REQUIRE(r.open(p));
    CHECK(r.header().formatMinor == kReplayFormatMinor + 7);
    CHECK(r.header().missionId == "from_the_future");
    REQUIRE(r.sections().factions.size() == 1);
    CHECK(r.sections().factions[0].name == "Red Force");
}

TEST_CASE("flrep refuses files that are not replays", "[replay]") {
    TempDir tmp("refuse");

    auto writeBytes = [&](const std::string& name, const std::vector<uint8_t>& b) {
        const fs::path p = tmp.path / name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return p;
    };

    SECTION("wrong magic") {
        ReplayReader r;
        CHECK_FALSE(r.open(writeBytes("bad.flrep", {'N', 'O', 'P', 'E', 1, 0, 0, 0})));
        CHECK(r.lastError().find("magic") != std::string::npos);
    }
    SECTION("empty file") {
        ReplayReader r;
        CHECK_FALSE(r.open(writeBytes("empty.flrep", {})));
    }
    SECTION("header truncated mid-field") {
        std::vector<uint8_t> b;
        b.insert(b.end(), std::begin(kReplayMagic), std::end(kReplayMagic));
        detail::putU16LE(b, kReplayFormatMajor);
        ReplayReader r;
        CHECK_FALSE(r.open(writeBytes("short.flrep", b)));
    }
    SECTION("missing file") {
        ReplayReader r;
        CHECK_FALSE(r.open(tmp.path / "does_not_exist.flrep"));
    }
    SECTION("impossible tick rate") {
        std::vector<uint8_t> b;
        b.insert(b.end(), std::begin(kReplayMagic), std::end(kReplayMagic));
        detail::putU16LE(b, kReplayFormatMajor);
        detail::putU16LE(b, kReplayFormatMinor);
        detail::putStringLE(b, "0.3.12");
        detail::putU32LE(b, 0); // a zero tick rate would make playback timing a division by zero
        detail::putF64LE(b, 6371000.0);
        detail::putU64LE(b, 0);
        detail::putStringLE(b, "");
        detail::putU32LE(b, 0);
        detail::putU32LE(b, 300);
        detail::putU16LE(b, 0);
        ReplayReader r;
        CHECK_FALSE(r.open(writeBytes("rate.flrep", b)));
        CHECK(r.lastError().find("tick rate") != std::string::npos);
    }
}

TEST_CASE("flrep does not allocate on a declared length it cannot back", "[replay]") {
    TempDir tmp("hostile");

    auto headerBytes = []() {
        std::vector<uint8_t> b;
        b.insert(b.end(), std::begin(kReplayMagic), std::end(kReplayMagic));
        detail::putU16LE(b, kReplayFormatMajor);
        detail::putU16LE(b, kReplayFormatMinor);
        detail::putStringLE(b, "0.3.12");
        detail::putU32LE(b, 60);
        detail::putF64LE(b, 6371000.0);
        detail::putU64LE(b, 0);
        detail::putStringLE(b, "");
        detail::putU32LE(b, 0);
        detail::putU32LE(b, 300);
        return b;
    };
    auto writeBytes = [&](const std::string& name, const std::vector<uint8_t>& b) {
        const fs::path p = tmp.path / name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return p;
    };

    SECTION("a section length past the end of the file") {
        std::vector<uint8_t> b = headerBytes();
        detail::putU16LE(b, static_cast<uint16_t>(ReplaySectionId::FactionTable));
        detail::putU32LE(b, 4u * 1024u * 1024u); // claims 4 MiB in a ~50-byte file
        ReplayReader r;
        CHECK_FALSE(r.open(writeBytes("bigsection.flrep", b)));
    }
    SECTION("an entity-type count far past the section it lives in") {
        std::vector<uint8_t> payload;
        detail::putU32LE(payload, 60000u); // claims 60k types; the section holds one
        detail::putU32LE(payload, 0);
        detail::putStringLE(payload, "only");
        detail::putStringLE(payload, "One");
        detail::putU8(payload, 0);
        detail::putU8(payload, 0);
        std::vector<uint8_t> b = headerBytes();
        detail::putU16LE(b, static_cast<uint16_t>(ReplaySectionId::EntityTypeManifest));
        detail::putU32LE(b, static_cast<uint32_t>(payload.size()));
        b.insert(b.end(), payload.begin(), payload.end());
        detail::putU16LE(b, 0u);
        ReplayReader r;
        CHECK_FALSE(r.open(writeBytes("bigcount.flrep", b))); // truncation is caught, not reserved on
    }
    SECTION("a string longer than the bytes that follow it") {
        std::vector<uint8_t> payload;
        detail::putU32LE(payload, 1);
        detail::putU16LE(payload, 0);
        detail::putU32LE(payload, 0xFFFFFF00u); // a 4 GiB faction id
        payload.push_back('x');
        std::vector<uint8_t> b = headerBytes();
        detail::putU16LE(b, static_cast<uint16_t>(ReplaySectionId::FactionTable));
        detail::putU32LE(b, static_cast<uint32_t>(payload.size()));
        b.insert(b.end(), payload.begin(), payload.end());
        detail::putU16LE(b, 0u);
        ReplayReader r;
        CHECK_FALSE(r.open(writeBytes("bigstring.flrep", b)));
    }
    SECTION("a chunk claiming an impossible decompressed size") {
        std::vector<uint8_t> b = headerBytes();
        detail::putU16LE(b, 0u);          // no sections
        detail::putU32LE(b, 0xF0000000u); // uncompressed bytes: way past the cap
        detail::putU32LE(b, 8u);
        detail::putU64LE(b, 0u);
        detail::putU32LE(b, 1u);
        for (int i = 0; i < 8; ++i)
            b.push_back(0);
        ReplayReader r;
        // No trailer either, so this exercises the scan path refusing the same claim.
        CHECK_FALSE(r.open(writeBytes("bigchunk.flrep", b)));
    }
}

TEST_CASE("flrep writes through a non-ASCII path", "[replay]") {
    TempDir tmp("unicode");
    // std::filesystem::path end to end is the point: InputTraceWriter's std::string ctor is exactly
    // the bug that makes a player with a non-ASCII profile directory unable to record.
    const fs::path dir = tmp.path / "réplays-日本";
    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(dir, "sessión")));
        REQUIRE(w.writeTick(makeTick(0, {makeEntity(0, 1.0, 2.0, 3.0)}, true)));
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(dir / "sessión.flrep"));
    ReplayTick t;
    CHECK(r.readNextTick(t));
}

TEST_CASE("flrep rotates by size and bounds the directory", "[replay]") {
    TempDir tmp("rotate");

    ReplayWriter::Config cfg = makeWriterConfig(tmp.path);
    cfg.maxFileBytes = 4096; // tiny, so a handful of ticks rolls the file
    cfg.maxFiles = 3;

    std::vector<QuantEntity> ents;
    for (uint32_t i = 0; i < 40; ++i)
        ents.push_back(makeEntity(i, 100.0 * i, 500.0, -50.0 * i));

    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), cfg));
        for (uint64_t i = 0; i < 60; ++i)
            REQUIRE(w.writeTick(makeTick(i, ents, (i % 2) == 0)));
        REQUIRE(w.close());
    }

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(tmp.path))
        if (e.path().extension() == ".flrep")
            files.push_back(e.path());

    // Rotation happened, and the directory bound held.
    CHECK(files.size() > 1);
    CHECK(files.size() <= cfg.maxFiles);

    // Every surviving file is independently readable -- a rotated file is a replay, not a fragment.
    for (const auto& p : files) {
        ReplayReader r;
        REQUIRE(r.open(p));
        ReplayTick t;
        CHECK(r.readNextTick(t));
    }
}

TEST_CASE("flrep rotation with maxFiles = 1 keeps exactly one readable file", "[replay]") {
    TempDir tmp("rotate1");
    ReplayWriter::Config cfg = makeWriterConfig(tmp.path);
    cfg.maxFileBytes = 2048;
    cfg.maxFiles = 1;

    std::vector<QuantEntity> ents;
    for (uint32_t i = 0; i < 30; ++i)
        ents.push_back(makeEntity(i, 10.0 * i, 500.0, 0.0));

    ReplayWriter w;
    REQUIRE(w.open(makeHeader(), makeSections(), cfg));
    for (uint64_t i = 0; i < 40; ++i)
        REQUIRE(w.writeTick(makeTick(i, ents, (i % 2) == 0)));
    REQUIRE(w.close());

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(tmp.path))
        if (e.path().extension() == ".flrep")
            files.push_back(e.path());

    // The degenerate case worth checking on its own: pruning must not delete the file it is writing
    // and leave nothing behind.
    REQUIRE(files.size() == 1);
    ReplayReader r;
    REQUIRE(r.open(files[0]));
    ReplayTick t;
    CHECK(r.readNextTick(t));
}

TEST_CASE("flrep tick encoding is stable under a random workload", "[replay]") {
    TempDir tmp("fuzzlite");
    std::mt19937 rng(20260727u); // fixed seed: a test that fails only sometimes teaches nothing
    std::uniform_real_distribution<double> posD(-200000.0, 200000.0);
    std::uniform_int_distribution<int> countD(0, 12);

    std::vector<ReplayTick> written;
    {
        ReplayWriter w;
        REQUIRE(w.open(makeHeader(), makeSections(), makeWriterConfig(tmp.path)));
        for (uint64_t i = 0; i < 50; ++i) {
            std::vector<QuantEntity> ents;
            const int n = countD(rng);
            for (int e = 0; e < n; ++e)
                ents.push_back(makeEntity(static_cast<uint32_t>(e), posD(rng), posD(rng), posD(rng)));
            ReplayTick t = makeTick(i, ents, (i % 5) == 0);
            written.push_back(t);
            REQUIRE(w.writeTick(t));
        }
        REQUIRE(w.close());
    }

    ReplayReader r;
    REQUIRE(r.open(tmp.path / "session.flrep"));
    for (const ReplayTick& expected : written) {
        ReplayTick got;
        REQUIRE(r.readNextTick(got));
        CHECK(got.tickIndex == expected.tickIndex);
        CHECK(got.recordCount == expected.recordCount);
        CHECK(got.flags == expected.flags);
        CHECK(got.origins == expected.origins);
        CHECK(got.records == expected.records); // byte-identical record stream
    }
}
