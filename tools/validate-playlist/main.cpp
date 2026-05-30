// SPDX-License-Identifier: GPL-3.0-or-later
#include "playlist_validator.h"

#include <cstdio>
#include <cstring>
#include <string>

static constexpr const char* kVersion = "0.0.1";

static void printHelp() {
    std::printf("Usage: validate-playlist --playlist <path/to/data/playlist.toml> [--pack <dir>]\n"
                "\n"
                "Validates a music playlist TOML file against the engine schema.\n"
                "With --pack, also verifies that each referenced OGG track exists on disk.\n"
                "\n"
                "Exit codes:\n"
                "  0  valid\n"
                "  1  validation errors found\n"
                "  2  bad arguments\n"
                "\n"
                "Options:\n"
                "  --playlist <file>  Path to data/playlist.toml (required)\n"
                "  --pack <dir>       Content pack root directory; enables track file checks\n"
                "  --help, -h         Show this help and exit\n"
                "  --version, -v      Show version and exit\n");
}

int main(int argc, char* argv[]) {
    std::string playlistPath;
    std::string packDir;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printHelp();
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("validate-playlist %s\n", kVersion);
            return 0;
        }
        if (std::strcmp(argv[i], "--playlist") == 0 && i + 1 < argc) {
            playlistPath = argv[++i];
        } else if (std::strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            packDir = argv[++i];
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (playlistPath.empty()) {
        std::fprintf(stderr, "error: --playlist is required\n");
        printHelp();
        return 2;
    }

    PlaylistValidationResult r = validatePlaylist(playlistPath, packDir);

    for (const auto& w : r.warnings)
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    for (const auto& e : r.errors)
        std::fprintf(stderr, "error: %s\n", e.c_str());

    if (r.ok()) {
        std::printf("OK: %d states, %d tracks\n", r.stateCount, r.totalTracks);
        return 0;
    }
    return 1;
}
