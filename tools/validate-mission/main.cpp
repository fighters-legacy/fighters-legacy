// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission_validator.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static constexpr const char* kVersion = "0.0.1";

static void printHelp() {
    std::printf("Usage: validate-mission <file.yaml> [file2.yaml ...]\n"
                "\n"
                "Validates YAML mission files against the schema in docs/modding/missions.md.\n"
                "All errors are reported in a single pass.\n"
                "\n"
                "Exit codes:\n"
                "  0  all files valid\n"
                "  1  one or more validation failures\n"
                "  2  bad arguments\n"
                "\n"
                "Options:\n"
                "  --help, -h     Show this help and exit\n"
                "  --version, -v  Show version and exit\n");
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
        std::printf("validate-mission %s\n", kVersion);
        return 0;
    }

    int exitCode = 0;
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
        auto result = validateMission(ss.str());
        for (const auto& w : result.warnings)
            std::fprintf(stderr, "WARN  [%s] %s\n", argv[i], w.c_str());
        for (const auto& e : result.errors)
            std::fprintf(stderr, "ERROR [%s] %s\n", argv[i], e.c_str());
        if (!result.ok)
            exitCode = 1;
    }
    return exitCode;
}
