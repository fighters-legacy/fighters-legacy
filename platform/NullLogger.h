// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The no-op ILogger, beside StdoutLogger.h and for the same reason NullAudio.h exists: a component
// that requires an ILogger& is not a component that requires log OUTPUT. The content validators
// drive the engine's real parsers, which log as they go, and a tool whose whole contract is its own
// stderr format must not have parser chatter interleaved into it.
//
// Header-only and dependency-free (part of platform-hal, which is INTERFACE), so tools, tests and
// the game share ONE null implementation rather than each keeping a copy that drifts as ILogger
// grows -- there were four, under three different names, before this header (#1277).
//
// Deliberately NOT `final`: a test double may derive from it and override only what it observes,
// the same way NullAudio, NullContentPack and NullNetwork work.

#include "ILogger.h"

namespace fl {

class NullLogger : public ILogger {
  public:
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

} // namespace fl
