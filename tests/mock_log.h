// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared ILogger test doubles. Kept out of mock_hal.h for the same reason mock_network.h is
// (see its lines 4-6): almost every suite that needs a logger needs nothing else from the HAL,
// and most of their targets link no HAL library at all. Naming mirrors mock_hal.h's convention —
// Null* = no-op, Recording* = records calls.
//
// Define a logger double HERE rather than re-declaring one per test file. Before this header there
// were 47 of them under 20 different names, which meant adding a pure virtual to ILogger was a
// 47-file edit and the "same" recorder had already forked into four incompatible shapes.

// fl::NullLogger is NOT here. It lives in platform/NullLogger.h beside StdoutLogger.h (#1277),
// because tools and the game want a no-op logger too and the tree must not hold two definitions of
// one type. This header re-exports it so a test needs a single include for both doubles.
#include "ILogger.h"
#include "NullLogger.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// Records every message so a test can assert on what was logged. Records ALL levels — the
// per-level filtering the hand-rolled copies did up front is a query here instead, so one recorder
// serves a suite that wants warnings and a suite that wants errors.
struct RecordingLogger : ILogger {
    struct Entry {
        LogLevel level;
        std::string message;
    };
    std::vector<Entry> entries;

    void log(LogLevel level, const char*, int, const char* message) override {
        entries.push_back({level, message ? message : ""});
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}

    // An empty needle matches every message, so count(level) reads as "how many at this level".
    [[nodiscard]] int count(LogLevel level, std::string_view needle = "") const {
        int n = 0;
        for (const auto& e : entries)
            if (e.level == level && e.message.find(needle) != std::string::npos)
                ++n;
        return n;
    }

    [[nodiscard]] bool hasMessage(LogLevel level, std::string_view needle = "") const {
        return count(level, needle) > 0;
    }

    // The messages logged at one level, in order — what the warnings-only copies exposed as a
    // bare vector member.
    [[nodiscard]] std::vector<std::string> messages(LogLevel level) const {
        std::vector<std::string> out;
        for (const auto& e : entries)
            if (e.level == level)
                out.push_back(e.message);
        return out;
    }
};

} // namespace fl
