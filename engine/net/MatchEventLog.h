// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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
// any thread -- AND, since #1077, the match event BUS. It used to stop one step short of that ("it does
// NOT replace the existing hooks"), and the disagreement its own comment predicted duly arrived:
// recordParticipant fired m_matchParticipantSink AND appended a Join/Leave, so every participant event
// was wired twice and every future event type would have been too. The tick-stamping bug (#1076) was
// the first divergence; nothing structural prevented the next.
//
// Now: subscribe() before the first append, notified synchronously with each record. The kills-only
// match sink, the participant sink and the observer half of the chat-intent hook are subscribers, and
// their WorldBroadcaster setters are gone. What deliberately stays OUT of the bus (D10):
//
//   * ChatModerationHook and TeamSwitchGuard return a decision -- they are VETOES, not observations.
//     An observer cannot refuse anything, so making them subscribers would silently drop the refusal.
//   * AdminChannel::dispatch returns a response body -- that is RPC, and a bus has nowhere to
//     put a reply.
//   * setReplaySink stays a dedicated high-bandwidth tap: a whole-world quantized stream is not an
//     event, and putting 60 Hz of world state through a per-record notification would be a category
//     error as well as a cost.
//
// Bounded on purpose: a long match must not grow this without limit, and an agent that stops reading
// must not be able to exhaust server memory. Old records are dropped, and `droppedCount()` says how
// many, so a consumer can tell "nothing happened" from "you fell behind".
//
// The LOG stamps `tick`, not the caller (#1076). It used to be the caller's field, and the outcome was
// the one an append-only log with a caller-supplied timestamp always reaches: two callers remembered
// and one did not, so every airspace alert-level change -- the escalation record #162 exists to
// produce -- was written at tick 0 in the .flrep recording, the /events stream and the MCP audit
// mirror. Three consumers, all wrong, silently, for every session recorded since Stage 1. There is no
// way for a reader to tell a real tick-0 event from a forgotten stamp, so the field cannot be left
// where forgetting it is possible. Same reasoning as `ServerUptime`: one authority for the value,
// required rather than defaulted, because a second source of truth IS the bug.

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
    uint64_t tick{0}; // sim tick the event occurred on. STAMPED BY MatchEventLog::append (#1076) --
                      // setting it on a record you are about to append has no effect. A record read
                      // back out of a .flrep or a JSON payload carries the tick the log stamped.

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

    // A bus subscriber. Called once per appended record, ON THE SIM THREAD, with the record as the log
    // stored it -- seq and tick already stamped, so a subscriber sees exactly what /events and the
    // .flrep will show.
    using Subscriber = std::function<void(const MatchEvent&)>;

    // Register before the sim starts. The list FREEZES at the first setTick() -- the same
    // freeze-at-start shape as EntityManager's handler list -- because a subscriber added mid-match
    // would silently miss everything before it with no way to know. Debug builds assert; release ignores
    // the late call rather than accepting a subscriber that begins with a hole in its history.
    //
    // The line is the sim START, not the first append, and the difference is load-bearing: server init
    // appends before the loop runs (a mission spawn is a Spawn record), and it does so BETWEEN the
    // subscriber registrations in fl-server's main -- the scoreboard is wired before the mission loads,
    // the chat-intent tier after. Freezing at the first append rejected the second one, for missing
    // spawn records no chat tier could want. Every event a subscriber exists for -- join, leave, kill,
    // chat, admin command, alert level -- happens after the loop starts, so this is where the honest
    // line falls.
    void subscribe(Subscriber fn);

    [[nodiscard]] std::size_t subscriberCount() const noexcept {
        return m_subscribers.size();
    }

    // Advance the log's tick. Called once per tick by the sim's owner (WorldBroadcaster::onTick),
    // before anything that could append. Sim thread.
    void setTick(uint64_t tick) {
        // Relaxed: this publishes no other data, and every append takes the mutex for the ring
        // itself. An append from another thread (the MCP audit path runs on an HTTP thread) may
        // therefore read a tick up to one tick stale, which is a far better number than the up-to-1 s
        // stale published-snapshot tick that path used to copy in by hand. Exact ORDERING is carried
        // by `seq`, which is stamped under the lock.
        m_tick.store(tick, std::memory_order_relaxed);
        // The sim is running, so the subscriber list is closed (#1077).
        m_subscribersFrozen = true;
        // ...and deliver anything an off-thread append parked. Here because this is the one sim-thread
        // call the log already receives every tick, so a subscriber needs no service() of its own and
        // cannot be starved by a quiet tick.
        drainDeferred();
    }

    [[nodiscard]] uint64_t tick() const noexcept {
        return m_tick.load(std::memory_order_relaxed);
    }

    // Stamps `seq` AND `tick`, then appends; the payload is the caller's. Any `tick` already set on
    // `ev` is overwritten -- that is the point, and it is what makes the stamp impossible to forget.
    // Callable from any thread.
    //
    // Subscribers are notified with the stored record, always on the SIM THREAD and never with the
    // ring's lock held (a subscriber may read the log). An append from another thread -- the MCP audit
    // path runs on an HTTP thread -- parks its record and the next setTick() delivers it, in seq order,
    // before that tick's own events. So a subscriber never has to be thread-aware, which is the whole
    // point of having one bus instead of five hooks: MatchController::recordKill is sim-thread-only,
    // and an off-thread notification would have been a race the old sinks did not have.
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

    // Off-thread records whose notification was dropped because the deferred queue was full. Non-zero
    // means a subscriber missed events; the RECORDS are still in the ring, so /events and the recorder
    // are unaffected. Reported rather than silent, for the same reason droppedCount() is.
    [[nodiscard]] uint64_t deferredNotifyDropped() const;

    void clear();

  private:
    // Deliver `rec` to every subscriber. Sim thread, lock NOT held.
    void notify(const MatchEvent& rec) const;
    // Deliver anything an off-thread append parked. Sim thread, lock NOT held.
    void drainDeferred();

    // Outside the mutex on purpose: the sim thread advances this every tick, and taking the ring's
    // lock at 60 Hz would contend with a reader copying up to 4096 records for a /events response.
    std::atomic<uint64_t> m_tick{0};
    // Frozen at the first append, so it is read without a lock on the notify path.
    std::vector<Subscriber> m_subscribers;
    bool m_subscribersFrozen{false};
    mutable std::mutex m_mutex;
    std::vector<MatchEvent> m_ring; // sized to m_capacity once; m_head is the next write slot
    std::size_t m_capacity;
    std::size_t m_head{0};
    std::size_t m_count{0};
    uint64_t m_nextSeq{1}; // seq 0 is reserved to mean "before everything"
    uint64_t m_dropped{0};
    // Records appended off the sim thread, awaiting notification at the next setTick(). Guarded by
    // m_mutex. Bounded by the same capacity as the ring: an agent hammering the audit path while the
    // sim is wedged must not grow this without limit, and a dropped notification is counted.
    std::vector<MatchEvent> m_deferred;
    uint64_t m_deferredDropped{0};
};

} // namespace fl
