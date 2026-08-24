// SPDX-License-Identifier: GPL-3.0-or-later
#include "mode_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace fl;

static void printHelp() {
    std::printf("Usage: validate-mode <file.toml> [file2.toml ...]\n"
                "\n"
                "Validates game-mode TOML files against the schema in docs/modding/game-modes.md.\n"
                "Delegates the schema to the runtime parser, then adds plausibility checks (duplicate\n"
                "team ids, unreachable score limit, warmup longer than the match clock, ...).\n"
                "All problems are reported in a single pass.\n"
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
    int exitCode = 0;
    if (handledHelpOrVersion(argv[1], "validate-mode", printHelp, exitCode))
        return exitCode;

    return runFileList(argc, argv, 1,
                       validateFileContents([](const std::string& contents) { return validateGameMode(contents); }));
}
