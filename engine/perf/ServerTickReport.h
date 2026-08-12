// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ServerTickReport — the serialised, transport-agnostic shape of the server tick budget.
//
// Produced by fl-server (from WorldBroadcaster::getTickBudget()) and written atomically to the
// --metrics-json file; consumed by bot_swarm (--server-metrics) which embeds it verbatim as the
// "server_tick" block in its report so the metric stays "one shape" on both sides. Header-only
// and dependency-free (just engine/perf + stdlib) so server/ and tools/ can both include it.
//
// fromJson() reads back through engine/perf/JsonScan.h — a deliberately small, tolerant scanner
// over the deterministic shape toJson() emits (missing/extra fields ignored, numbers via strtod).
// Those scanners were promoted out of this header when FrameStatsRecorder.h needed them too.

#include "JsonScan.h"
#include "Stats.h"
#include "TickProfiler.h"
#include "util/Json.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// v2 (#514) adds the overrun-governor load_factor + the GameLoop dropped_ticks counter.
// v3 (#707) adds self-reported process RSS (rss_kb + rss_startup_kb) for the soak leak gate.
// v4 (#726) adds the overrun-governor interest_scale (interest-radius shed lever).
// v5 (#714) adds the congestion-controller run-long watermarks (congestion_min_send_hz +
//           congestion_recovered_send_hz + congestion_max_loss) for the synthetic congestion gate.
// v6 (#772) adds WIRE traffic (wire_out_kbs / wire_in_kbs / wire_out_pps / wire_out_kbs_per_client):
//           bytes actually on the socket, including transport framing and (GNS) AES-GCM overhead.
//           Distinct from the swarm's `downstream_kbs_per_client`, which counts application payload
//           and is transport-independent by construction — it cannot see what a transport costs to
//           run, which is the number an operator's bandwidth bill is denominated in.
//
// ── FROZEN AT 6 FOR THE REST OF PRIMARY DEVELOPMENT (#686) ────────────────────────────────────────
// The sensing phase (#685) added `sensing_ms` and did NOT bump this, and neither should the next
// additive change. The number was being bumped ritually, and it bought nothing:
//
//   * NOTHING GATES ON IT. fl-server writes it, fromJson parses it into a field, and no consumer has
//     ever compared it against anything. It was a version on a door nobody checks.
//   * Both sides of the "contract" live in this repo and land in the same commit — fl-server writes
//     the file, bot_swarm and scale_gate.py read it. There is no third party to be compatible with,
//     and no old reader in the wild to protect (same reasoning that keeps kProtocolVersion at 1).
//   * The format is ADDITIVE and NAME-KEYED: toJson/fromJson iterate the phase table, and every
//     consumer looks fields up by name and ignores what it does not recognise. A new phase or field
//     simply appears; an old reader keeps working. That is what makes the version redundant, and it
//     is a property worth preserving deliberately rather than a happy accident.
//
// So: DO NOT bump this for a new phase, a new stat, or a new field. Bump it only if a field's
// MEANING changes under an unchanged name, or one is removed — the cases where a reader would
// silently misinterpret a file rather than merely miss something. Near release, when metrics files
// start outliving the binary that wrote them, this becomes a real compatibility contract again and
// the bumping discipline comes back with it. This reasoning is now decision record D18
// (docs/developer/architecture.md): a format carries a CHECKED version iff it crosses a machine or
// build boundary; an in-repo producer/consumer pair freezes the number instead.
inline constexpr int kServerTickSchemaVersion = 6;

struct ServerTickReport {
    int schemaVersion{kServerTickSchemaVersion};
    double tickHz{0.0};
    uint64_t ticksSampled{0};
    uint64_t ticksTotal{0};
    double windowSeconds{0.0};
    int peers{0};
    uint32_t entities{0};
    // World object ceiling + what it has refused (#1049). ADDITIVE, no schema bump (see the freeze
    // note above): entity_soft_cap is 0 when uncapped, and a non-zero entity_cap_refusals is the
    // machine-readable evidence that the cap is actually binding — the thing an operator could not
    // see at all while the key was parsed but unwired.
    uint32_t entitySoftCap{0};
    uint64_t entityCapRefusals{0};
    // Voice relay fan-out (#1090). ADDITIVE and name-keyed, so no schema bump (D18 / the freeze note
    // above). The FAN-OUT, not the frame count, is what voice costs the server: a frame is relayed to
    // every listener on the net, so a net costs (talkers x listeners). This is the number that makes
    // the concurrent-speaker cap's effect visible to an operator instead of a claim in a doc.
    uint64_t voiceRelaySends{0};
    std::array<Stats, kTickPhaseCount> phases{}; // indexed by TickPhase
    Stats total{};
    Stats other{};
    double loadFactor{1.0};    // overrun governor: [floor, 1]; 1 = no degradation (#514)
    double interestScale{1.0}; // overrun governor interest-radius scale: [fraction floor, 1] (#726)
    uint64_t droppedTicks{0};  // all-time GameLoop catch-up drops (sim overrun / time dilation) (#514)
    uint64_t rssKb{0};         // current process resident set size, KiB; 0 = unavailable (#707)
    uint64_t rssStartupKb{0};  // RSS captured once after init; the soak leak gate tracks the delta (#707)
    // Congestion-controller run-long watermarks (#714/#518); frozen while no peers are connected.
    double congestionMinSendHz{60.0};       // all-time min adaptive send rate across peers; 60 = never engaged
    double congestionRecoveredSendHz{60.0}; // max send rate observed since the min was set (recovery evidence)
    double congestionMaxLoss{0.0};          // all-time max sampled ENet mean loss fraction (diagnostic)
    // Wire traffic (#772) — socket bytes, NOT application payload. 0 on backends that don't report it.
    double wireOutKbs{0.0};           // host egress KB/s
    double wireInKbs{0.0};            // host ingress KB/s
    double wireOutPacketsPerSec{0.0}; // host egress datagrams/s
    int wirePeers{0};                 // peers connected when the wire sample was taken (NOT the live `peers` count)

    // Per-peer throttle attribution (#576). Optional and ADDITIVE: an empty vector emits no
    // `peer_throttle` key at all, so every existing consumer and every golden file is unchanged.
    //
    // NO SCHEMA BUMP. kServerTickSchemaVersion is frozen at 6 (#686) and the rule is to bump only
    // when a field's MEANING changes under an unchanged name. This is a new, name-keyed array whose
    // producer and consumer land together — exactly the case the freeze was written for. (#576's own
    // text says "schema version bump per its versioning rules"; those rules say don't.)
    struct PeerThrottle {
        uint32_t peerId{0};
        double sendRateHz{60.0};
        uint32_t intervalTicks{1};
        bool governorBinding{false};   // the SERVER's overrun governor is spacing this peer
        bool congestionBinding{false}; // this peer's own link is
        double packetLoss{0.0};
    };
    std::vector<PeerThrottle> peerThrottle;

    // Egress per connected client, KB/s — the per-client comparable of `downstream_kbs_per_client`,
    // but wire rather than payload.
    //
    // Divides by `wirePeers`, NOT the live `peers`: the wire sample is frozen at the last tick that
    // had peers, while `peers` is whatever is connected when the metrics file is written — which,
    // for the gate's end-of-run snapshot, is zero. Dividing by the live count produced 0.00 KB/s and
    // silently destroyed the measurement.
    double wireOutKbsPerClient() const {
        return wirePeers > 0 ? wireOutKbs / static_cast<double>(wirePeers) : 0.0;
    }
};

// Build a report from a profiler snapshot plus the live peer/entity counts and overrun state.
inline ServerTickReport makeServerTickReport(const TickBudget& b, int peers, uint32_t entities, double loadFactor = 1.0,
                                             uint64_t droppedTicks = 0, uint64_t rssKb = 0, uint64_t rssStartupKb = 0,
                                             double interestScale = 1.0, double congestionMinSendHz = 60.0,
                                             double congestionRecoveredSendHz = 60.0, double congestionMaxLoss = 0.0,
                                             double wireOutKbs = 0.0, double wireInKbs = 0.0,
                                             double wireOutPacketsPerSec = 0.0, int wirePeers = 0,
                                             uint32_t entitySoftCap = 0, uint64_t entityCapRefusals = 0) {
    ServerTickReport r;
    r.tickHz = b.tickHz;
    r.ticksSampled = b.ticksSampled;
    r.ticksTotal = b.ticksTotal;
    r.windowSeconds = b.windowSeconds;
    r.peers = peers;
    r.entities = entities;
    r.phases = b.phases;
    r.total = b.total;
    r.other = b.other;
    r.loadFactor = loadFactor;
    r.interestScale = interestScale;
    r.droppedTicks = droppedTicks;
    r.rssKb = rssKb;
    r.rssStartupKb = rssStartupKb;
    r.congestionMinSendHz = congestionMinSendHz;
    r.congestionRecoveredSendHz = congestionRecoveredSendHz;
    r.congestionMaxLoss = congestionMaxLoss;
    r.wireOutKbs = wireOutKbs;
    r.wireInKbs = wireInKbs;
    r.wireOutPacketsPerSec = wireOutPacketsPerSec;
    r.wirePeers = wirePeers;
    r.entitySoftCap = entitySoftCap;
    r.entityCapRefusals = entityCapRefusals;
    return r;
}

namespace detail {

inline std::string statJson(const char* name, const Stats& s, const std::string& indent) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%s\"%s\": { \"min\": %.4f, \"mean\": %.4f, \"max\": %.4f, \"p95\": %.4f, \"p99\": %.4f }",
                  indent.c_str(), name, s.min, s.mean, s.max, s.p95, s.p99);
    return buf;
}

} // namespace detail

// Serialises the report as a JSON object. `indentSpaces` shifts every line (for nesting inside
// another document, e.g. the bot_swarm "server_tick" block).
inline std::string toJson(const ServerTickReport& r, int indentSpaces = 0) {
    const std::string pad(static_cast<std::size_t>(indentSpaces < 0 ? 0 : indentSpaces), ' ');
    const std::string in = pad + "  ";
    char head[1280];
    std::snprintf(head, sizeof(head),
                  "%s{\n"
                  "%s\"schema_version\": %d,\n"
                  "%s\"tick_hz\": %.4f,\n"
                  "%s\"ticks_sampled\": %llu, \"ticks_total\": %llu,\n"
                  "%s\"window_s\": %.4f,\n"
                  "%s\"peers\": %d, \"entities\": %u, \"entity_soft_cap\": %u, \"entity_cap_refusals\": %llu,\n"
                  "%s\"voice_relay_sends\": %llu,\n"
                  "%s\"load_factor\": %.4f, \"interest_scale\": %.4f, \"dropped_ticks\": %llu,\n"
                  "%s\"rss_kb\": %llu, \"rss_startup_kb\": %llu,\n"
                  "%s\"congestion_min_send_hz\": %.4f, \"congestion_recovered_send_hz\": %.4f, "
                  "\"congestion_max_loss\": %.4f,\n"
                  "%s\"wire_out_kbs\": %.4f, \"wire_in_kbs\": %.4f, \"wire_out_pps\": %.4f, "
                  "\"wire_peers\": %d, \"wire_out_kbs_per_client\": %.4f,\n",
                  pad.c_str(), in.c_str(), r.schemaVersion, in.c_str(), r.tickHz, in.c_str(),
                  static_cast<unsigned long long>(r.ticksSampled), static_cast<unsigned long long>(r.ticksTotal),
                  in.c_str(), r.windowSeconds, in.c_str(), r.peers, r.entities, r.entitySoftCap,
                  static_cast<unsigned long long>(r.entityCapRefusals), in.c_str(),
                  static_cast<unsigned long long>(r.voiceRelaySends), in.c_str(), r.loadFactor, r.interestScale,
                  static_cast<unsigned long long>(r.droppedTicks), in.c_str(), static_cast<unsigned long long>(r.rssKb),
                  static_cast<unsigned long long>(r.rssStartupKb), in.c_str(), r.congestionMinSendHz,
                  r.congestionRecoveredSendHz, r.congestionMaxLoss, in.c_str(), r.wireOutKbs, r.wireInKbs,
                  r.wireOutPacketsPerSec, r.wirePeers, r.wireOutKbsPerClient());
    std::string out = head;
    out += detail::statJson("tick_ms", r.total, in) + ",\n";
    for (int i = 0; i < kTickPhaseCount; ++i) {
        const std::string name = std::string(tickPhaseName(static_cast<TickPhase>(i))) + "_ms";
        out += detail::statJson(name.c_str(), r.phases[i], in) + ",\n";
    }
    out += detail::statJson("other_ms", r.other, in);
    // #576: per-peer attribution, emitted only when there is any. Ordered by the caller (ascending
    // peerId) so two runs of the same session produce byte-comparable files.
    if (!r.peerThrottle.empty()) {
        out += ",\n" + in + "\"peer_throttle\": [\n";
        for (std::size_t i = 0; i < r.peerThrottle.size(); ++i) {
            const auto& pt = r.peerThrottle[i];
            char row[256];
            std::snprintf(row, sizeof(row),
                          "%s  {\"peer\": %u, \"send_hz\": %.4f, \"interval_ticks\": %u, "
                          "\"lever\": \"%s\", \"loss\": %.4f}%s\n",
                          in.c_str(), pt.peerId, pt.sendRateHz, pt.intervalTicks,
                          pt.governorBinding ? "server" : (pt.congestionBinding ? "link" : "none"), pt.packetLoss,
                          i + 1 < r.peerThrottle.size() ? "," : "");
            out += row;
        }
        out += in + "]\n";
    } else {
        out += "\n";
    }
    out += pad + "}";
    return out;
}

// Parses a report from JSON (tolerant). Returns false if no recognisable fields were found.
//
// Accepts EITHER a bare tick report or a document that embeds one under "server_tick" — which is what
// bot_swarm's report does, and what scale_gate.py reads. That used to work by accident: the old
// find-based reader located a key at any depth, so handing it a swarm report happened to find the
// nested fields. It also meant an outer field sharing a name with an inner one silently won by document
// order. The unified structural reader (#1080) looks at one object level, so the nesting is stated here
// instead of being a property of the scanner.
inline bool fromJson(std::string_view document, ServerTickReport& out) {
    const std::string_view embedded = json::member(document, "server_tick");
    const std::string_view json = json::isObject(embedded) ? embedded : document;
    bool any = false;
    if (auto v = json::numberField(json, "schema_version")) {
        out.schemaVersion = static_cast<int>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "tick_hz")) {
        out.tickHz = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "ticks_sampled")) {
        out.ticksSampled = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "ticks_total")) {
        out.ticksTotal = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "window_s")) {
        out.windowSeconds = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "peers")) {
        out.peers = static_cast<int>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "voice_relay_sends"))
        out.voiceRelaySends = static_cast<uint64_t>(*v);
    if (auto v = json::numberField(json, "entities")) {
        out.entities = static_cast<uint32_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "entity_soft_cap")) {
        out.entitySoftCap = static_cast<uint32_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "entity_cap_refusals")) {
        out.entityCapRefusals = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "load_factor")) {
        out.loadFactor = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "interest_scale")) {
        out.interestScale = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "dropped_ticks")) {
        out.droppedTicks = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "rss_kb")) {
        out.rssKb = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "rss_startup_kb")) {
        out.rssStartupKb = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = json::numberField(json, "congestion_min_send_hz")) {
        out.congestionMinSendHz = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "congestion_recovered_send_hz")) {
        out.congestionRecoveredSendHz = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "congestion_max_loss")) {
        out.congestionMaxLoss = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "wire_out_kbs")) {
        out.wireOutKbs = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "wire_in_kbs")) {
        out.wireInKbs = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "wire_out_pps")) {
        out.wireOutPacketsPerSec = *v;
        any = true;
    }
    if (auto v = json::numberField(json, "wire_peers")) {
        out.wirePeers = static_cast<int>(*v);
        any = true;
    }
    // wire_out_kbs_per_client is DERIVED (wireOutKbs / peers) and deliberately not parsed back:
    // it is emitted for the gate/human reader, and re-deriving it keeps one source of truth.
    any |= detail::parseStat(json, "tick_ms", out.total);
    for (int i = 0; i < kTickPhaseCount; ++i) {
        const std::string name = std::string(tickPhaseName(static_cast<TickPhase>(i))) + "_ms";
        any |= detail::parseStat(json, name.c_str(), out.phases[i]);
    }
    any |= detail::parseStat(json, "other_ms", out.other);
    return any;
}

// Reads + parses a metrics file. Returns nullopt on missing/empty/unparseable input.
inline std::optional<ServerTickReport> loadServerMetrics(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty())
        return std::nullopt;
    ServerTickReport r;
    if (!fromJson(content, r))
        return std::nullopt;
    return r;
}

} // namespace fl
