// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The perf-report-specific read helper: one Stats sub-object, read by name.
//
// The general scanners that used to live here -- findNumber / findString -- were three of the five
// independent JSON readers #1080 consolidated. They are gone; `engine/util/Json.h` carries the one
// reader now, and it is the STRUCTURAL one. These reports have nested objects (`"tick_ms": { "p99": .. }`),
// and the old find-based lookup matched a key at ANY depth: `findNumber(json, "min")` on a whole tick
// report answered with the first `"min"` in the document, whichever sub-object it belonged to. Reading
// the sub-object first and then its member is both correct and what the caller meant.

#include "Stats.h"
#include "util/Json.h"

#include <string_view>

namespace fl::detail {

// Parse the Stats sub-object that follows `key`, filling `out`. Returns true if the key held an
// object. Fields absent from that object keep their existing values, so the format stays additive.
inline bool parseStat(std::string_view json, std::string_view key, Stats& out) {
    const std::string_view obj = json::member(json, key);
    if (!json::isObject(obj))
        return false;
    if (auto v = json::numberField(obj, "min"))
        out.min = *v;
    if (auto v = json::numberField(obj, "mean"))
        out.mean = *v;
    if (auto v = json::numberField(obj, "max"))
        out.max = *v;
    if (auto v = json::numberField(obj, "p95"))
        out.p95 = *v;
    if (auto v = json::numberField(obj, "p99"))
        out.p99 = *v;
    if (auto v = json::numberField(obj, "stddev"))
        out.stddev = *v;
    return true;
}

} // namespace fl::detail
