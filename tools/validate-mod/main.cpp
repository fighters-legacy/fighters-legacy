// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void printUsage() {
    std::printf("Usage: validate-mod [--no-licenses] [--allow <spdx-id>]... <pack-dir>\n"
                "\n"
                "Validates a whole content pack: the manifest, an optional [files] sha256 table, the\n"
                "pack structure, and every asset through the per-asset validators (entities, weapons,\n"
                "sensors, meshes, flight models, liveries, missions, campaigns, theaters, playlist,\n"
                "airports) plus REUSE license compliance.\n"
                "\n"
                "Exit codes: 0 = valid, 1 = validation failures, 2 = bad arguments.\n"
                "  --no-licenses      Skip the REUSE license check\n"
                "  --allow <spdx-id>  Allow an extra SPDX id (repeatable; e.g. --allow MIT)\n"
                "  --help, -h         Show this help and exit\n"
                "  --version, -v      Show version and exit\n");
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    fl::ModValidateOptions opts;
    std::string packDir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage();
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("validate-mod %s\n", fl::kValidatorVersion);
            return 0;
        }
        if (std::strcmp(argv[i], "--no-licenses") == 0) {
            opts.checkLicenses = false;
        } else if (std::strcmp(argv[i], "--allow") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--allow requires an SPDX id\n");
                return 2;
            }
            opts.allowedSpdx.push_back(argv[++i]);
        } else if (packDir.empty()) {
            packDir = argv[i];
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (packDir.empty()) {
        printUsage();
        return 2;
    }

    // No [file] label: a whole-pack run reports against the pack, and each message names its own
    // asset.
    int exitCode = 0;
    fl::reportResult(fl::validateMod(packDir, opts), nullptr, exitCode);
    return exitCode;
}
