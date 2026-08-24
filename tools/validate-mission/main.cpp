// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace fl;

static void printHelp() {
    std::printf("Usage: validate-mission [--pack <dir>] <file.yaml> [file2.yaml ...]\n"
                "\n"
                "Validates YAML mission files against the schema in docs/modding/missions.md.\n"
                "All errors are reported in a single pass.\n"
                "\n"
                "With --pack, each object's crew: block is cross-checked against the referenced\n"
                "entity type's declared [[crew]] seats in the pack (a seat/role the entity does\n"
                "not declare is an error).\n"
                "\n"
                "Exit codes:\n"
                "  0  all files valid\n"
                "  1  one or more validation failures\n"
                "  2  bad arguments\n"
                "\n"
                "Options:\n"
                "  --pack <dir>   Cross-check crew: blocks against the pack's entities/*.toml\n"
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
    if (handledHelpOrVersion(argv[1], "validate-mission", printHelp, exitCode))
        return exitCode;

    // --pack may appear anywhere in the list and applies to every file after it, so this one keeps
    // its own loop rather than using runFileList.
    std::string packDir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pack") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --pack takes a directory\n");
                return 2;
            }
            packDir = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
        const std::optional<std::string> contents = readFileToString(argv[i]);
        if (!contents) {
            std::fprintf(stderr, "error: cannot open %s\n", argv[i]);
            exitCode = 1;
            continue;
        }
        reportResult(packDir.empty() ? validateMission(*contents) : validateMission(*contents, packDir), argv[i],
                     exitCode);
    }
    return exitCode;
}
