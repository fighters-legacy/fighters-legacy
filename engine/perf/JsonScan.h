// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// JsonScan — the tolerant read side of the engine's perf-report JSON documents.
//
// Promoted out of engine/perf/ServerTickReport.h when FrameStatsRecorder.h (#782) needed the same
// scanners: two copies of a hand-rolled JSON reader is two things to keep in agreement, and they
// would disagree eventually.
//
// These are NOT a JSON parser. They are a small scanner over the deterministic shape the matching
// toJson() emits — enough to read a value back by name, tolerant of missing/extra/reordered fields,
// and nothing more. That tolerance is the point: the perf-report formats are additive and
// name-keyed (see the #686 note in ServerTickReport.h), so a reader must ignore what it does not
// recognise rather than fail on it.
//
// Numbers go through strtod, NOT std::from_chars — Apple Clang has no floating-point from_chars.

#include "Stats.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace fl::detail {

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

// Returns the string value following the first `"key"` occurrence, or nullopt. Unescapes only the
// escape sequences the matching writers emit: quote, backslash, newline, CR and tab.
inline std::optional<std::string> findString(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto kpos = json.find(needle);
    if (kpos == std::string_view::npos)
        return std::nullopt;
    const auto cpos = json.find(':', kpos + needle.size());
    if (cpos == std::string_view::npos)
        return std::nullopt;
    const auto open = json.find('"', cpos + 1);
    if (open == std::string_view::npos)
        return std::nullopt;
    std::string out;
    for (std::size_t i = open + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"')
            return out;
        if (c == '\\' && i + 1 < json.size()) {
            const char n = json[++i];
            switch (n) {
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            default:
                // Covers the escaped quote and the escaped backslash.
                out += n;
            }
            continue;
        }
        out += c;
    }
    return std::nullopt; // unterminated string
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
    if (auto v = findNumber(obj, "stddev"))
        out.stddev = *v;
    return true;
}

} // namespace fl::detail
