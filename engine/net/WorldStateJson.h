// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/MatchEventLog.h"
#include "net/WorldState.h"
#include "util/Json.h"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Deterministic JSON for the world-state snapshot and the match event log (#600).
//
// Hand-rolled, in the ServerTickReport.h style, for the reason that file already gives: this stays
// dependency-free, and engine-net must not acquire a JSON library it would then have to keep out of
// engine-protocol. Header-only and inline; the caller decides which thread pays for the string
// building, and at ~1 Hz that is deliberately not the sim thread.
//
// The format is ADDITIVE and NAME-KEYED. New fields append; consumers look up by name and ignore
// what they do not know. There is deliberately no schema_version here — nothing gates on one, and a
// version nobody checks is a ritual, not a contract (decision record D18,
// docs/developer/architecture.md: this format crosses no machine or build boundary).
//
// UNLIKE MissionReport.h, this escapes strings. It carries chat lines, admin command text and
// faction names, which are attacker-controlled or mod-controlled; emitting them raw would let a chat
// line containing a quote break the document, which is a JSON-injection bug and not a cosmetic one.
//
// The escaper and the two writer helpers this file used to define were promoted to engine/util/Json.h
// under #1080 -- they were shared correctly by all of fl-server already, which is what showed the
// consolidation works, so they became the shared ones rather than one of two copies.

namespace fl {

// One entity as a compact single-line object — entity lists run to thousands of rows, and one line
// per row keeps a snapshot readable in a terminal and diffable in a golden test.
[[nodiscard]] inline std::string toJson(const WorldStateEntity& e) {
    std::string s = "{ \"idx\": " + std::to_string(e.entityIdx) + ", \"gen\": " + std::to_string(e.gen) +
                    ", \"faction\": " + std::to_string(e.factionIndex) + ", \"type\": " + std::to_string(e.typeIndex) +
                    ", \"owner_peer\": " + std::to_string(e.ownerPeerId) +
                    ", \"formation\": " + std::to_string(e.formationId) +
                    ", \"category\": " + std::to_string(e.category) +
                    ", \"damage_level\": " + std::to_string(e.damageLevel) + ", \"flags\": " + std::to_string(e.flags) +
                    ", \"pos\": [" + json::num(e.pos[0]) + ", " + json::num(e.pos[1]) + ", " + json::num(e.pos[2]) +
                    "], \"vel\": [" + json::num(e.vel[0]) + ", " + json::num(e.vel[1]) + ", " + json::num(e.vel[2]) +
                    "], \"hp_frac\": " + json::num(e.hpFrac) + ", \"sweep_deg\": " + json::num(e.sweepDeg) + " }";
    return s;
}

[[nodiscard]] inline std::string toJson(const WorldStatePeer& p) {
    return "{ \"peer_id\": " + std::to_string(p.peerId) + ", \"faction\": " + std::to_string(p.factionIndex) +
           ", \"delay_ticks\": " + std::to_string(p.delayTicks) + ", \"role\": " + std::to_string(p.role) + " }";
}

[[nodiscard]] inline std::string toJson(const WorldStateFaction& f) {
    return "{ \"index\": " + std::to_string(f.factionIndex) + ", \"id\": " + json::str(f.id) +
           ", \"name\": " + json::str(f.name) + ", \"alert_level\": " + std::to_string(f.alertLevel) + " }";
}

[[nodiscard]] inline std::string toJson(const MatchEvent& e) {
    std::string s =
        "{ \"seq\": " + std::to_string(e.seq) + ", \"tick\": " + std::to_string(e.tick) +
        ", \"type\": " + json::str(matchEventTypeName(e.type)) + ", \"subject_idx\": " + std::to_string(e.subjectIdx) +
        ", \"subject_gen\": " + std::to_string(e.subjectGen) +
        ", \"instigator_idx\": " + std::to_string(e.instigatorIdx) +
        ", \"instigator_gen\": " + std::to_string(e.instigatorGen) + ", \"actor\": " + std::to_string(e.actor) +
        ", \"target\": " + std::to_string(e.target) + ", \"faction\": " + std::to_string(e.factionIndex) +
        ", \"weapon_class\": " + std::to_string(e.weaponClass) + ", \"channel\": " + std::to_string(e.channel) +
        ", \"value\": " + std::to_string(e.value);
    // `text` is omitted rather than emitted empty: most record types never carry one, and a snapshot
    // of a busy match is mostly kills and spawns.
    if (!e.text.empty())
        s += ", \"text\": " + json::str(e.text);
    s += " }";
    return s;
}

namespace detail {

// Emit `rows` as a JSON array, one element per line, indented by `in`. Handles the trailing-comma
// rule in one place so each caller does not re-derive it.
template <typename T, typename Fn>
[[nodiscard]] inline std::string jsonArray(const T& rows, const std::string& in, Fn&& emit) {
    if (rows.empty())
        return "[]";
    std::string s = "[\n";
    std::size_t i = 0;
    for (const auto& row : rows) {
        s += in + "  " + emit(row);
        s += (++i == rows.size()) ? "\n" : ",\n";
    }
    s += in + "]";
    return s;
}

} // namespace detail

// The world-state snapshot. `indentSpaces` shifts every line so the object nests inside a larger
// document (the ServerTickReport convention).
[[nodiscard]] inline std::string toJson(const WorldStateSnapshot& w, int indentSpaces = 0) {
    const std::string pad(static_cast<std::size_t>(indentSpaces < 0 ? 0 : indentSpaces), ' ');
    const std::string in = pad + "  ";

    std::string s = pad + "{\n";
    s += in + "\"tick\": " + std::to_string(w.tick) + ",\n";
    s += in + "\"weather_preset\": " + std::to_string(w.weatherPreset) + ",\n";
    s += in + "\"time_of_day_hours\": " + json::num(w.timeOfDayHours) + ",\n";
    s += in + "\"wind\": [" + json::num(w.windX) + ", " + json::num(w.windZ) + "],\n";

    s += in + "\"mission\": { \"active\": " + std::string(w.mission.active ? "true" : "false") +
         ", \"name\": " + json::str(w.mission.name) + ", \"outcome\": " + std::to_string(w.mission.outcome) +
         ", \"triggers_fired\": " + std::to_string(w.mission.triggersFired) +
         ", \"elapsed_seconds\": " + json::num(w.mission.elapsedSeconds) + " },\n";

    s += in +
         "\"factions\": " + detail::jsonArray(w.factions, in, [](const WorldStateFaction& f) { return toJson(f); }) +
         ",\n";

    // The relationship matrix as an array of rows, so a reader indexes [a][b] the same way the
    // struct's relationship() accessor does.
    s += in + "\"relationships\": ";
    const std::size_t n = w.factions.size();
    if (n == 0 || w.relationships.size() != n * n) {
        s += "[]";
    } else {
        s += "[\n";
        for (std::size_t a = 0; a < n; ++a) {
            s += in + "  [";
            for (std::size_t b = 0; b < n; ++b) {
                s += std::to_string(w.relationships[a * n + b]);
                if (b + 1 != n)
                    s += ", ";
            }
            s += "]";
            s += (a + 1 == n) ? "\n" : ",\n";
        }
        s += in + "]";
    }
    s += ",\n";

    s += in + "\"peers\": " + detail::jsonArray(w.peers, in, [](const WorldStatePeer& p) { return toJson(p); }) + ",\n";
    s += in +
         "\"entities\": " + detail::jsonArray(w.entities, in, [](const WorldStateEntity& e) { return toJson(e); }) +
         "\n";
    s += pad + "}";
    return s;
}

// A batch of events, with the cursor a consumer needs to resume. `nextSeq` is what it should pass as
// `after` next time; `gap` says records it has not seen were already dropped, so it can react rather
// than believe it has a complete history.
[[nodiscard]] inline std::string matchEventsToJson(std::span<const MatchEvent> events, uint64_t nextSeq, bool gap,
                                                   int indentSpaces = 0) {
    const std::string pad(static_cast<std::size_t>(indentSpaces < 0 ? 0 : indentSpaces), ' ');
    const std::string in = pad + "  ";

    std::string s = pad + "{\n";
    s += in + "\"next_seq\": " + std::to_string(nextSeq) + ",\n";
    s += in + "\"gap\": " + std::string(gap ? "true" : "false") + ",\n";
    s += in + "\"count\": " + std::to_string(events.size()) + ",\n";
    s += in + "\"events\": " + detail::jsonArray(events, in, [](const MatchEvent& e) { return toJson(e); }) + "\n";
    s += pad + "}";
    return s;
}

} // namespace fl
