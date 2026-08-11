// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/MatchEventLog.h"
#include "net/WorldStateJson.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace fl;

namespace {

[[nodiscard]] MatchEvent ev(MatchEventType t) {
    MatchEvent e;
    e.type = t;
    return e;
}

// append() stamps `tick` from the log (#1076), so a test that wants a record at tick N advances the
// log to N first. That is the production call order: WorldBroadcaster::onTick sets the tick, then
// anything in that tick appends.
void appendAtTick(MatchEventLog& log, MatchEventType t, uint64_t tick) {
    log.setTick(tick);
    log.append(ev(t));
}

} // namespace

// ── the log is the tick authority (#1076) ───────────────────────────────────────────────────────

// The defect this replaced: `append()` stamped seq but left `tick` to its callers. Two remembered;
// the AlertLevel caller in fl-server/main.cpp did not, so every airspace posture change -- the
// escalation record #162 exists to produce -- was written at tick 0 in the .flrep recording, the
// /events stream and the MCP audit mirror. A reader cannot tell a real tick-0 event from a forgotten
// stamp, so the fix is not "remember to stamp it" but "the caller cannot stamp it".
TEST_CASE("MatchEventLog: a record appended with no tick set carries the log's tick, not 0", "[match_event_log]") {
    MatchEventLog log(16);
    log.setTick(4321);

    MatchEvent alert; // exactly the AlertLevel caller's shape: type, faction, value, no tick
    alert.type = MatchEventType::AlertLevel;
    alert.factionIndex = 2;
    alert.value = 3;
    log.append(std::move(alert));

    const auto all = log.since(0);
    REQUIRE(all.size() == 1);
    CHECK(all[0].tick == 4321); // 0 before #1076
    CHECK(all[0].type == MatchEventType::AlertLevel);
}

TEST_CASE("MatchEventLog: append overwrites a tick the caller set, so there is one authority", "[match_event_log]") {
    MatchEventLog log(16);
    log.setTick(100);

    MatchEvent stale;
    stale.type = MatchEventType::Kill;
    stale.tick = 7; // a caller that stamps is now simply ignored rather than trusted
    log.append(std::move(stale));

    const auto all = log.since(0);
    REQUIRE(all.size() == 1);
    CHECK(all[0].tick == 100);
}

TEST_CASE("MatchEventLog: the tick starts at 0 and each append takes the tick current at that moment",
          "[match_event_log]") {
    MatchEventLog log(16);
    CHECK(log.tick() == 0);

    log.append(ev(MatchEventType::Spawn)); // pre-first-tick: 0 is the honest answer here
    log.setTick(10);
    log.append(ev(MatchEventType::Spawn));
    log.setTick(11);
    log.append(ev(MatchEventType::Despawn));

    const auto all = log.since(0);
    REQUIRE(all.size() == 3);
    CHECK(all[0].tick == 0);
    CHECK(all[1].tick == 10);
    CHECK(all[2].tick == 11);
}

// ── ring semantics ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("MatchEventLog: seq is monotonic from 1 and records come back oldest-first", "[match_event_log]") {
    MatchEventLog log(16);
    CHECK(log.size() == 0);
    CHECK(log.nextSeq() == 1); // seq 0 is reserved for "before everything"

    for (uint64_t i = 0; i < 5; ++i)
        appendAtTick(log, MatchEventType::Spawn, i);

    CHECK(log.size() == 5);
    CHECK(log.nextSeq() == 6);

    const auto all = log.since(0);
    REQUIRE(all.size() == 5);
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK(all[i].seq == i + 1);
        CHECK(all[i].tick == i);
    }
}

TEST_CASE("MatchEventLog: since(N) returns only what the caller has not seen", "[match_event_log]") {
    MatchEventLog log(16);
    for (uint64_t i = 0; i < 5; ++i)
        appendAtTick(log, MatchEventType::Kill, i);

    const auto after3 = log.since(3);
    REQUIRE(after3.size() == 2);
    CHECK(after3[0].seq == 4);
    CHECK(after3[1].seq == 5);

    CHECK(log.since(5).empty()); // fully caught up
    CHECK(log.since(999).empty());
}

TEST_CASE("MatchEventLog: the ring is bounded and reports what it dropped", "[match_event_log]") {
    MatchEventLog log(4);
    for (uint64_t i = 0; i < 10; ++i)
        appendAtTick(log, MatchEventType::Spawn, i);

    CHECK(log.size() == 4); // never grows past capacity
    CHECK(log.capacity() == 4);
    CHECK(log.droppedCount() == 6);
    CHECK(log.nextSeq() == 11);

    // Only the newest four survive, still oldest-first.
    const auto all = log.since(0);
    REQUIRE(all.size() == 4);
    CHECK(all[0].seq == 7);
    CHECK(all[3].seq == 10);
    CHECK(all[0].tick == 6);
}

TEST_CASE("MatchEventLog: a consumer that fell behind can detect the gap", "[match_event_log]") {
    MatchEventLog log(4);
    for (uint64_t i = 0; i < 10; ++i)
        appendAtTick(log, MatchEventType::Spawn, i);

    // A consumer resuming at seq 2 expects seq 3 next; the oldest retained is 7, so records are gone.
    // Without this it would receive 7..10 and believe it had a complete history.
    CHECK(log.hasGapBefore(2));
    CHECK(log.hasGapBefore(5));
    CHECK_FALSE(log.hasGapBefore(6)); // expects 7 next, and 7 is the oldest retained
    CHECK_FALSE(log.hasGapBefore(9));

    MatchEventLog fresh(16);
    CHECK_FALSE(fresh.hasGapBefore(0)); // an empty log that dropped nothing has no gap
}

TEST_CASE("MatchEventLog: tail returns the newest N, oldest-first", "[match_event_log]") {
    MatchEventLog log(16);
    for (uint64_t i = 0; i < 8; ++i)
        appendAtTick(log, MatchEventType::Chat, i);

    const auto t3 = log.tail(3);
    REQUIRE(t3.size() == 3);
    CHECK(t3[0].seq == 6);
    CHECK(t3[2].seq == 8);

    CHECK(log.tail(100).size() == 8); // clamped to what is retained
    CHECK(log.tail(0).empty());
}

TEST_CASE("MatchEventLog: clear drops records but never rewinds seq", "[match_event_log]") {
    MatchEventLog log(8);
    for (uint64_t i = 0; i < 3; ++i)
        appendAtTick(log, MatchEventType::Spawn, i);
    const uint64_t seqBefore = log.nextSeq();

    log.clear();
    CHECK(log.size() == 0);
    CHECK(log.droppedCount() == 0);
    // Rewinding seq would let a consumer resuming at N silently receive DIFFERENT events under seq
    // numbers it had already processed.
    CHECK(log.nextSeq() == seqBefore);

    appendAtTick(log, MatchEventType::Kill, 99);
    CHECK(log.since(0)[0].seq == seqBefore);
}

TEST_CASE("MatchEventLog: concurrent appends and reads are safe and lose nothing", "[match_event_log]") {
    // The log is written on the sim thread and read by REST/MCP/the recorder on theirs; this is the
    // case that motivates the lock. Run under TSan in CI.
    MatchEventLog log(4096);
    constexpr int kPerThread = 500;
    constexpr int kWriters = 4;

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w)
        writers.emplace_back([&log] {
            for (int i = 0; i < kPerThread; ++i)
                log.append(ev(MatchEventType::Spawn));
        });

    // A dedicated ticker models production exactly (#1076): ONE thread advances the tick -- the sim
    // thread, from WorldBroadcaster::onTick -- while appends arrive from others, including the MCP
    // audit path on an HTTP thread. That concurrency is what makes the tick an atomic instead of a
    // plain member under the ring's mutex, so it is the thing TSan needs to see.
    std::atomic<bool> ticking{true};
    std::thread ticker([&log, &ticking] {
        for (uint64_t t = 0; ticking.load(std::memory_order_relaxed); ++t)
            log.setTick(t);
    });

    std::thread reader([&log] {
        for (int i = 0; i < 200; ++i) {
            const auto batch = log.since(0);
            (void)log.tail(16);
            (void)log.tick();
            (void)batch.size();
        }
    });

    for (auto& t : writers)
        t.join();
    reader.join();
    ticking.store(false, std::memory_order_relaxed);
    ticker.join();

    CHECK(log.nextSeq() == static_cast<uint64_t>(kWriters * kPerThread) + 1);
    CHECK(log.size() == static_cast<std::size_t>(kWriters * kPerThread));

    // Every seq from 1..N appears exactly once — no append was lost or duplicated under contention.
    const auto all = log.since(0);
    REQUIRE(all.size() == static_cast<std::size_t>(kWriters * kPerThread));
    for (std::size_t i = 0; i < all.size(); ++i)
        CHECK(all[i].seq == i + 1);
}

TEST_CASE("MatchEventLog: the type vocabulary round-trips and gates untrusted ordinals", "[match_event_log]") {
    CHECK(std::string(matchEventTypeName(MatchEventType::Kill)) == "kill");
    CHECK(std::string(matchEventTypeName(MatchEventType::AdminCommand)) == "admin_command");
    CHECK(std::string(matchEventTypeName(MatchEventType::AlertLevel)) == "alert_level");
    CHECK(isMatchEventTypeOrdinal(0));
    CHECK(isMatchEventTypeOrdinal(static_cast<uint8_t>(MatchEventType::AgentAction)));
    CHECK_FALSE(isMatchEventTypeOrdinal(static_cast<uint8_t>(MatchEventType::AgentAction) + 1));
}

// ── JSON ────────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("jsonEscape closes the injection a raw chat line would open", "[match_event_log][json]") {
    // MissionReport.h assumes safe input and says so; this document carries chat text and admin
    // commands, which a player controls.
    CHECK(jsonEscape("plain") == "plain");
    CHECK(jsonEscape("say \"hi\"") == "say \\\"hi\\\"");
    CHECK(jsonEscape("back\\slash") == "back\\\\slash");
    CHECK(jsonEscape("a\nb\tc\rd") == "a\\nb\\tc\\rd");
    // Split literal on purpose: "\x01end" would greedily parse as the single hex escape \x1e.
    CHECK(jsonEscape(std::string("ctl\x01"
                                 "end")) == "ctl\\u0001end");
    // A UTF-8 multibyte sequence passes through untouched.
    CHECK(jsonEscape("caf\xc3\xa9") == "caf\xc3\xa9");
}

TEST_CASE("match event JSON carries the cursor and escapes its text", "[match_event_log][json]") {
    MatchEventLog log(16);
    log.setTick(1234);
    MatchEvent kill;
    kill.type = MatchEventType::Kill;
    kill.subjectIdx = 7;
    kill.subjectGen = 2;
    kill.instigatorIdx = 3;
    kill.actor = 11;
    kill.target = 22;
    kill.weaponClass = 4;
    log.append(std::move(kill));

    log.setTick(1240);
    MatchEvent chat;
    chat.type = MatchEventType::Chat;
    chat.actor = 11;
    chat.text = "he said \"break right\"";
    log.append(std::move(chat));

    const auto evs = log.since(0);
    const std::string json = matchEventsToJson(std::span<const MatchEvent>(evs), log.nextSeq(),
                                               /*gap=*/false);

    // Schema stability is asserted by key presence, the ServerTickReport idiom.
    CHECK(json.find("\"next_seq\": 3") != std::string::npos);
    CHECK(json.find("\"gap\": false") != std::string::npos);
    CHECK(json.find("\"count\": 2") != std::string::npos);
    CHECK(json.find("\"type\": \"kill\"") != std::string::npos);
    CHECK(json.find("\"type\": \"chat\"") != std::string::npos);
    CHECK(json.find("\"weapon_class\": 4") != std::string::npos);
    CHECK(json.find("\"tick\": 1234") != std::string::npos);

    // The quotes in the chat line are escaped, not emitted raw.
    CHECK(json.find("\\\"break right\\\"") != std::string::npos);
    CHECK(json.find("he said \"break right\"") == std::string::npos);

    // A record with no text omits the key rather than emitting an empty one.
    const std::string killOnly = toJson(evs[0]);
    CHECK(killOnly.find("\"text\"") == std::string::npos);
}

TEST_CASE("match event JSON is empty-safe and reports a gap", "[match_event_log][json]") {
    const std::vector<MatchEvent> none;
    const std::string json = matchEventsToJson(std::span<const MatchEvent>(none), 1, /*gap=*/true);
    CHECK(json.find("\"count\": 0") != std::string::npos);
    CHECK(json.find("\"events\": []") != std::string::npos);
    CHECK(json.find("\"gap\": true") != std::string::npos);
}
