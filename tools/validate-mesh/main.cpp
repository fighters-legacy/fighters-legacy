// SPDX-License-Identifier: GPL-3.0-or-later
#include "mesh_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace fl;

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
    int exitCode = 0;
    if (handledHelpOrVersion(argv[1], "validate-mesh", printHelp, exitCode))
        return exitCode;

    // A .glb is binary and tinygltf opens it itself, so this one validates a PATH rather than
    // contents -- and reports without a [file] label, as it always has.
    return runFileList(argc, argv, 1,
                       [](const char* path, int& code) { reportResult(validateMesh(path), nullptr, code); });
}
