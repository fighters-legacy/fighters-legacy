// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon_validator.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace fl;

static constexpr const char* kVersion = "0.0.1";

static void printHelp() {
    std::printf("Usage: validate-weapon <file.toml> [file2.toml ...]\n"
                "       validate-weapon --pack <pack-dir>\n"
                "\n"
                "Validates weapon TOML files against the schema in docs/modding/formats.md,\n"
                "using the engine's own parser — a weapon this tool passes is a weapon the\n"
                "engine loads. Also warns about values that are legal but implausible.\n"
                "\n"
                "With --pack, validates every weapons/*.toml in a content pack and rejects\n"
                "duplicate weapon ids. The hardpoint-to-weapon cross-check lives in\n"
                "validate-entity --pack (the references are in entity files).\n"
                "\n"
                "Exit codes:\n"
                "  0  all files valid\n"
                "  1  one or more validation failures\n"
                "  2  bad arguments\n"
                "\n"
                "Options:\n"
                "  --pack <dir>   Validate every weapon definition in a content pack\n"
                "  --help, -h     Show this help and exit\n"
                "  --version, -v  Show version and exit\n");
}

static void report(const WeaponValidationResult& result, const char* label, int& exitCode) {
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
        std::printf("validate-weapon %s\n", kVersion);
        return 0;
    }

    int exitCode = 0;

    if (std::strcmp(argv[1], "--pack") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "error: --pack takes exactly one directory\n");
            return 2;
        }
        report(validatePackWeapons(argv[2]), argv[2], exitCode);
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
        report(validateWeapon(ss.str()), argv[i], exitCode);
    }
    return exitCode;
}
