// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: parsePlaylist — the music-playlist TOML parser (toml++) that ingests a content
// pack's data/playlist.toml. It logs (does not throw) on error and needs an ILogger&; a silent one
// is supplied. Invariant: no OOB read / no UB in the parse for any attacker-controlled TOML bytes.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ILogger.h"
#include "audio/PlaylistLoader.h"

namespace {
struct SilentLogger : fl::ILogger {
    void log(fl::LogLevel, const char*, int, const char*) override {}
    void setMinLevel(fl::LogLevel) override {}
    void flush() override {}
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    SilentLogger logger;
    std::string_view src(reinterpret_cast<const char*>(data), size);
    (void)fl::parsePlaylist(src, logger);
    return 0;
}
