// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ServerTickReport — the serialised, transport-agnostic shape of the server tick budget.
//
// Produced by fl-server (from WorldBroadcaster::getTickBudget()) and written atomically to the
// --metrics-json file; consumed by bot_swarm (--server-metrics) which embeds it verbatim as the
// "server_tick" block in its report so the metric stays "one shape" on both sides. Header-only
// and dependency-free (just engine/perf + stdlib) so server/ and tools/ can both include it.
//
// fromJson() is a deliberately small, tolerant scanner over the deterministic shape toJson()
// emits — missing/extra fields are ignored, numbers parsed with strtod (NOT std::from_chars,
// which lacks floating-point support on Apple Clang).

#include "Stats.h"
#include "TickProfiler.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace fl {

inline constexpr int kServerTickSchemaVersion = 1;

struct ServerTickReport {
    int schemaVersion{kServerTickSchemaVersion};
    double tickHz{0.0};
    uint64_t ticksSampled{0};
    uint64_t ticksTotal{0};
    double windowSeconds{0.0};
    int peers{0};
    uint32_t entities{0};
    std::array<Stats, kTickPhaseCount> phases{}; // indexed by TickPhase
    Stats total{};
    Stats other{};
};

// Build a report from a profiler snapshot plus the live peer/entity counts.
inline ServerTickReport makeServerTickReport(const TickBudget& b, int peers, uint32_t entities) {
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

// Returns the numeric value following the first `"key"` occurrence (after its colon), or nullopt.
inline std::optional<double> findNumber(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto kpos = json.find(needle);
    if (kpos == std::string_view::npos)
        return std::nullopt;
    auto cpos = json.find(':', kpos + needle.size());
    if (cpos == std::string_view::npos)
        return std::nullopt;
    // Copy the tail to a NUL-terminated buffer for strtod (string_view is not guaranteed NUL-terminated).
    std::string tail(json.substr(cpos + 1));
    char* end = nullptr;
    const double v = std::strtod(tail.c_str(), &end);
    if (end == tail.c_str())
        return std::nullopt;
    return v;
}

// Parses the stat sub-object that follows `"key"` (its `{ ... }`), filling out. Returns true if
// the key (and an object body) was found.
inline bool parseStat(std::string_view json, std::string_view key, Stats& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto kpos = json.find(needle);
    if (kpos == std::string_view::npos)
        return false;
    const auto open = json.find('{', kpos);
    if (open == std::string_view::npos)
        return false;
    const auto close = json.find('}', open);
    if (close == std::string_view::npos)
        return false;
    const std::string_view obj = json.substr(open, close - open + 1);
    if (auto v = findNumber(obj, "min"))
        out.min = *v;
    if (auto v = findNumber(obj, "mean"))
        out.mean = *v;
    if (auto v = findNumber(obj, "max"))
        out.max = *v;
    if (auto v = findNumber(obj, "p95"))
        out.p95 = *v;
    if (auto v = findNumber(obj, "p99"))
        out.p99 = *v;
    return true;
}

} // namespace detail

// Serialises the report as a JSON object. `indentSpaces` shifts every line (for nesting inside
// another document, e.g. the bot_swarm "server_tick" block).
inline std::string toJson(const ServerTickReport& r, int indentSpaces = 0) {
    const std::string pad(static_cast<std::size_t>(indentSpaces < 0 ? 0 : indentSpaces), ' ');
    const std::string in = pad + "  ";
    char head[512];
    std::snprintf(head, sizeof(head),
                  "%s{\n"
                  "%s\"schema_version\": %d,\n"
                  "%s\"tick_hz\": %.4f,\n"
                  "%s\"ticks_sampled\": %llu, \"ticks_total\": %llu,\n"
                  "%s\"window_s\": %.4f,\n"
                  "%s\"peers\": %d, \"entities\": %u,\n",
                  pad.c_str(), in.c_str(), r.schemaVersion, in.c_str(), r.tickHz, in.c_str(),
                  static_cast<unsigned long long>(r.ticksSampled), static_cast<unsigned long long>(r.ticksTotal),
                  in.c_str(), r.windowSeconds, in.c_str(), r.peers, r.entities);
    std::string out = head;
    out += detail::statJson("tick_ms", r.total, in) + ",\n";
    for (int i = 0; i < kTickPhaseCount; ++i) {
        const std::string name = std::string(tickPhaseName(static_cast<TickPhase>(i))) + "_ms";
        out += detail::statJson(name.c_str(), r.phases[i], in) + ",\n";
    }
    out += detail::statJson("other_ms", r.other, in) + "\n";
    out += pad + "}";
    return out;
}

// Parses a report from JSON (tolerant). Returns false if no recognisable fields were found.
inline bool fromJson(std::string_view json, ServerTickReport& out) {
    bool any = false;
    if (auto v = detail::findNumber(json, "schema_version")) {
        out.schemaVersion = static_cast<int>(*v);
        any = true;
    }
    if (auto v = detail::findNumber(json, "tick_hz")) {
        out.tickHz = *v;
        any = true;
    }
    if (auto v = detail::findNumber(json, "ticks_sampled")) {
        out.ticksSampled = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = detail::findNumber(json, "ticks_total")) {
        out.ticksTotal = static_cast<uint64_t>(*v);
        any = true;
    }
    if (auto v = detail::findNumber(json, "window_s")) {
        out.windowSeconds = *v;
        any = true;
    }
    if (auto v = detail::findNumber(json, "peers")) {
        out.peers = static_cast<int>(*v);
        any = true;
    }
    if (auto v = detail::findNumber(json, "entities")) {
        out.entities = static_cast<uint32_t>(*v);
        any = true;
    }
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
