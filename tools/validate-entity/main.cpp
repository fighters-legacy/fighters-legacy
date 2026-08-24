// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace fl;

static void printHelp() {
    std::printf("Usage: validate-entity <file.toml> [file2.toml ...]\n"
                "       validate-entity --pack <pack-dir>\n"
                "\n"
                "Validates entity TOML files with the engine's own parser — an entity this tool\n"
                "passes is an entity the engine registers.\n"
                "\n"
                "With --pack, resolves every reference against the pack the way the engine does:\n"
                "asset names (mesh, cockpit, flight_model, ai_script, damage_mesh, manual) must\n"
                "name files the pack actually has, and def ids (sensors, hardpoint weapons) must\n"
                "resolve through the id index. An unresolvable flight_model is an ERROR: at\n"
                "runtime it silently degrades to the builtin model and the aircraft flies wrong\n"
                "rather than not at all.\n"
                "\n"
                "Single-file mode parses and warns about silent fallbacks only — references can\n"
                "only be resolved against a pack.\n"
                "\n"
                "Exit codes:\n"
                "  0  all files valid\n"
                "  1  one or more validation failures\n"
                "  2  bad arguments\n"
                "\n"
                "Options:\n"
                "  --pack <dir>   Validate every entities/*.toml in a content pack, with\n"
                "                 cross-file reference resolution\n"
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
    if (handledHelpOrVersion(argv[1], "validate-entity", printHelp, exitCode))
        return exitCode;

    if (std::strcmp(argv[1], "--pack") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "error: --pack takes exactly one directory\n");
            return 2;
        }
        reportResult(validateEntityPack(argv[2]), argv[2], exitCode);
        return exitCode;
    }

    return runFileList(argc, argv, 1,
                       validateFileContents([](const std::string& contents) { return validateEntity(contents); }));
}
