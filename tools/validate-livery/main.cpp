// SPDX-License-Identifier: GPL-3.0-or-later
#include "livery_validator.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace fl;

static constexpr const char* kVersion = "0.0.1";

static void printHelp() {
    std::printf("Usage: validate-livery <file.toml> [file2.toml ...]\n"
                "       validate-livery --pack <pack-dir>\n"
                "\n"
                "Validates livery TOML files with the engine's own parser — a livery this tool\n"
                "passes is a livery the engine loads. A livery re-skins an aircraft by material\n"
                "slot; it never touches geometry, nodes or UVs.\n"
                "\n"
                "With --pack, resolves references the way the engine does: each texture asset\n"
                "name must name a texture file the pack actually has (ERROR — the livery ships\n"
                "its own skins), and the aircraft def id should resolve to an entity in the pack\n"
                "(WARN only — a livery pack legitimately targets an aircraft from a base pack).\n"
                "\n"
                "Single-file mode parses and warns about plausibility only — references can only\n"
                "be resolved against a pack.\n"
                "\n"
                "Exit codes:\n"
                "  0  all files valid\n"
                "  1  one or more validation failures\n"
                "  2  bad arguments\n"
                "\n"
                "Options:\n"
                "  --pack <dir>   Validate every liveries/*.toml in a content pack, with\n"
                "                 cross-file reference resolution\n"
                "  --help, -h     Show this help and exit\n"
                "  --version, -v  Show version and exit\n");
}

static void report(const LiveryValidationResult& result, const char* label, int& exitCode) {
    for (const auto& w : result.warnings)
        std::fprintf(stderr, "WARN  [%s] %s\n", label, w.c_str());
    for (const auto& e : result.errors)
        std::fprintf(stderr, "ERROR [%s] %s\n", label, e.c_str());
    if (!result.ok)
        exitCode = 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "error: no input files\n");
        printHelp();
        return 2;
    }
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        printHelp();
        return 0;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0) {
        std::printf("validate-livery %s\n", kVersion);
        return 0;
    }

    int exitCode = 0;

    if (std::strcmp(argv[1], "--pack") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "error: --pack takes exactly one directory\n");
            return 2;
        }
        report(validateLiveryPack(argv[2]), argv[2], exitCode);
        return exitCode;
    }

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
        std::ifstream f(argv[i]);
        if (!f) {
            std::fprintf(stderr, "error: cannot open %s\n", argv[i]);
            exitCode = 1;
            continue;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        report(validateLivery(ss.str()), argv[i], exitCode);
    }
    return exitCode;
}
