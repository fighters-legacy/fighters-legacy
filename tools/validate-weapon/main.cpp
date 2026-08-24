// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace fl;

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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "error: no input files\n");
        printHelp();
        return 2;
    }
    int exitCode = 0;
    if (handledHelpOrVersion(argv[1], "validate-weapon", printHelp, exitCode))
        return exitCode;

    if (std::strcmp(argv[1], "--pack") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "error: --pack takes exactly one directory\n");
            return 2;
        }
        reportResult(validatePackWeapons(argv[2]), argv[2], exitCode);
        return exitCode;
    }

    return runFileList(argc, argv, 1,
                       validateFileContents([](const std::string& contents) { return validateWeapon(contents); }));
}
