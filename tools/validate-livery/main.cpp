// SPDX-License-Identifier: GPL-3.0-or-later
#include "livery_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace fl;

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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "error: no input files\n");
        printHelp();
        return 2;
    }
    int exitCode = 0;
    if (handledHelpOrVersion(argv[1], "validate-livery", printHelp, exitCode))
        return exitCode;

    if (std::strcmp(argv[1], "--pack") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "error: --pack takes exactly one directory\n");
            return 2;
        }
        reportResult(validateLiveryPack(argv[2]), argv[2], exitCode);
        return exitCode;
    }

    return runFileList(argc, argv, 1,
                       validateFileContents([](const std::string& contents) { return validateLivery(contents); }));
}
