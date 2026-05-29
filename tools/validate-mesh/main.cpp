// SPDX-License-Identifier: GPL-3.0-or-later
#include "mesh_validator.h"

#include <cstdio>
#include <cstring>
#include <string>

static constexpr const char* kVersion = "0.0.1";

static void printHelp() {
    std::printf("Usage: validate-mesh <file.glb> [file2.gltf ...]\n"
                "\n"
                "Validates glTF 2.0 files against engine mesh conventions documented in\n"
                "docs/modding/3d-models.md. LOD sibling files (e.g. fa18c_lod0.glb) are\n"
                "discovered and validated automatically.\n"
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
        std::printf("validate-mesh %s\n", kVersion);
        return 0;
    }

    int exitCode = 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
        auto result = validateMesh(argv[i]);
        for (const auto& w : result.warnings)
            std::fprintf(stderr, "WARN  %s\n", w.c_str());
        for (const auto& e : result.errors)
            std::fprintf(stderr, "ERROR %s\n", e.c_str());
        if (!result.ok)
            exitCode = 1;
    }
    return exitCode;
}
