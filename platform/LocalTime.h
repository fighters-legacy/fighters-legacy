// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>

namespace fl {

// Broken-down LOCAL time, portably, in one place (#1265).
//
// std::localtime returns a pointer into a shared static buffer and is not thread-safe, so every
// caller has to use the reentrant variant — which is spelled localtime_s on Windows (with the
// arguments the other way round) and localtime_r everywhere else. Five sites carried the same
// three-line #ifdef: the crash reporter, the replay-select screen, FileLogger twice, and the
// server's replay filename stamp.
//
// Home is platform/ root because the copies span engine, game, platform and server, and platform
// may never include engine (architecture.md) — so this is the only directory all four can reach.
// The Utf8Decode.h precedent is the same shape.
//
// The FORMAT stays at each call site: a log line, a filename and a crash stamp want different
// strings, and that difference is the callers', not this function's.
[[nodiscard]] inline std::tm localTimeBreakdown(std::time_t t) noexcept {
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

} // namespace fl
