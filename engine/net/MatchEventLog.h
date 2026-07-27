// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// MatchEventLog (#600) — one append-only record of everything that happened in a match.
//
// Before this, five disjoint hooks each carried a slice of "what happened": a kills-only
// MatchEventSink, a join/leave MatchParticipantSink, a chat moderation VETO, an admin-command RPC
// whose only trace was a log line, and IEntityEventHandler. Spawns were observable nowhere. Three
// separate consumers were about to need the same information -- the #600 event stream, the #643
// replay recorder, and the #601 agent audit mirror -- and building three bespoke event paths to
// serve them is how a codebase ends up with five hooks that disagree.
//
// So: one bounded ring of typed, tick-stamped records, written on the sim thread and readable from
// any thread. It does NOT replace the existing hooks. Two of them are not observers at all -- the
// chat hook returns a veto and the admin dispatch returns a response body -- so the log records what
// flowed through them rather than standing in for them.
//
// Bounded on purpose: a long match must not grow this without limit, and an agent that stops reading
// must not be able to exhaust server memory. Old records are dropped, and `droppedCount()` says how
// many, so a consumer can tell "nothing happened" from "you fell behind".

namespace fl {

enum class MatchEventType : uint8_t {
    Spawn = 0,        // an entity entered the sim (#600 adds EntityEventType::Spawned to raise it)
    Despawn = 1,      // an entity was removed without being killed
    Kill = 2,         // subject died; instigator credited
    DamageLevel = 3,  // subject's damage level changed (value = new DamageLevel ordinal)
    Score = 4,        // score credited to actor (value = points)
    Join = 5,         // a participant joined the match
    Leave = 6,        // a participant left
    Chat = 7,         // a chat line passed moderation (text = the sanitized line)
    AdminCommand = 8, // an admin command was dispatched (text = the command; actor = the issuer)
    AlertLevel = 9,   // a faction's airspace posture changed (#162; value = AlertLevel ordinal)
    AgentAction = 10, // an MCP/agent tool invocation (#601), recorded for the audit mirror
};

// Both inline: the `.flrep` reader (#643) needs the ordinal gate and the type name while living in
// engine-replay, which must not link engine-net -- the game client reads replays and has no business
// pulling in the server sim.
[[nodiscard]] inline const char* matchEventTypeName(MatchEventType t) noexcept {
    switch (t) {
    case MatchEventType::Spawn:
        return "spawn";
    case MatchEventType::Despawn:
        return "despawn";
    case MatchEventType::Kill:
        return "kill";
    case MatchEventType::DamageLevel:
        return "damage_level";
    case MatchEventType::Score:
        return "score";
    case MatchEventType::Join:
        return "join";
    case MatchEventType::Leave:
        return "leave";
    case MatchEventType::Chat:
        return "chat";
    case MatchEventType::AdminCommand:
        return "admin_command";
    case MatchEventType::AlertLevel:
        return "alert_level";
    case MatchEventType::AgentAction:
        return "agent_action";
    }
    return "unknown";
}

// Gate an ordinal that arrived from outside (a replay file, a wire message) before casting.
[[nodiscard]] inline bool isMatchEventTypeOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(MatchEventType::AgentAction);
}

// One record. Deliberately a flat POD-plus-string rather than a variant: every consumer here is a
// serializer, and a flat record serializes without a visitor. Fields not meaningful for a type are
// left at their defaults, and `matchEventTypeName` documents which is which.
struct MatchEvent {
    uint64_t seq{0};  // monotonic across the session; a consumer resumes with "everything after N"
    uint64_t tick{0}; // sim tick the event occurred on

    MatchEventType type{MatchEventType::Spawn};

    uint32_t subjectIdx{0}; // entity this is about (pool index)
    uint16_t subjectGen{0};
    uint32_t instigatorIdx{kNoEntityIdx}; // who caused it; kNoEntityIdx = environment / none
    uint16_t instigatorGen{0};

    uint32_t actor{kNoParticipant};  // participant id: killer, chatter, admin issuer, joiner
    uint32_t target{kNoParticipant}; // participant id: victim
    uint16_t factionIndex{0};

    uint8_t weaponClass{0}; // Kill: the WeaponClass ordinal, as the reliable kill feed carries
    uint8_t channel{0};     // Chat: ChatChannel ordinal
    int32_t value{0};       // Score: points. DamageLevel/AlertLevel: the new ordinal.

    std::string text; // Chat line, admin command, agent tool name. ATTACKER-CONTROLLED -- any
                      // serializer must escape it (see WorldStateJson.h).

    static constexpr uint32_t kNoEntityIdx = 0xFFFFFFFFu;
    static constexpr uint32_t kNoParticipant = 0xFFFFFFFFu;
};

// Bounded append-only ring. Sim thread appends; the world-state API, the replay recorder and the
// agent audit mirror read from other threads, so every method takes the lock. At match-event rates
// (a handful per second, not per tick) that contention is irrelevant, and a mutex here is honestly
// race-free rather than a lock-free structure that needs a TSan suppression to look clean.
class MatchEventLog {
  public:
    static constexpr std::size_t kDefaultCapacity = 4096;

    explicit MatchEventLog(std::size_t capacity = kDefaultCapacity);

    // Stamps `seq` and appends. `tick` and the payload are the caller's. Sim thread.
    void append(MatchEvent ev);

    // Every retained record with seq > afterSeq, oldest first. Pass 0 for "everything retained".
    // Returns copies: a caller serializing to JSON on another thread must not hold the lock while
    // it does so, and must not hold pointers into a ring that keeps being written.
    [[nodiscard]] std::vector<MatchEvent> since(uint64_t afterSeq) const;

    // The most recent `count` records, oldest first -- the "tail" a REST/MCP read wants by default.
    [[nodiscard]] std::vector<MatchEvent> tail(std::size_t count) const;

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept {
        return m_capacity;
    }
    // seq of the next record to be appended; also the count of everything ever appended.
    [[nodiscard]] uint64_t nextSeq() const;
    // How many records were dropped off the front. Non-zero means a consumer asking `since(N)` for
    // an N older than the retained window has a GAP -- which it can detect, rather than silently
    // receiving a partial history it believes is complete.
    [[nodiscard]] uint64_t droppedCount() const;
    // True when `afterSeq` is older than the oldest retained record, i.e. `since(afterSeq)` cannot
    // return a complete answer.
    [[nodiscard]] bool hasGapBefore(uint64_t afterSeq) const;

    void clear();

  private:
    mutable std::mutex m_mutex;
    std::vector<MatchEvent> m_ring; // sized to m_capacity once; m_head is the next write slot
    std::size_t m_capacity;
    std::size_t m_head{0};
    std::size_t m_count{0};
    uint64_t m_nextSeq{1}; // seq 0 is reserved to mean "before everything"
    uint64_t m_dropped{0};
};

} // namespace fl
