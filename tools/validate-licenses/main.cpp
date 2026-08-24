// SPDX-License-Identifier: GPL-3.0-or-later
#include "license_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

static void printHelp() {
    std::printf("Usage: validate-licenses [--dir <path>] [--licenses-dir <path>] [--allow <id>] ...\n"
                "\n"
                "Validates REUSE 1.0 license compliance for a content pack directory.\n"
                "\n"
                "Options:\n"
                "  --dir <path>          Directory to scan (default: current directory)\n"
                "  --licenses-dir <path> Path to LICENSES/ directory (default: <dir>/LICENSES)\n"
                "  --allow <spdx-id>     Allowed SPDX identifier (repeatable)\n"
                "                        Default: CC0-1.0, CC-BY-4.0\n"
                "  --help, -h            Show this help and exit\n"
                "  --version, -v         Show version and exit\n"
                "\n"
                "Exit codes:\n"
                "  0  all checks pass\n"
                "  1  one or more validation failures\n"
                "  2  bad arguments\n");
}

int main(int argc, char* argv[]) {
    int exitCode = 0;
    if (argc >= 2 && handledHelpOrVersion(argv[1], "validate-licenses", printHelp, exitCode))
        return exitCode;

    std::string dir = ".";
    std::string licensesDir;
    std::vector<std::string> allowedIds;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --dir requires an argument\n");
                return 2;
            }
            dir = argv[++i];
        } else if (std::strcmp(argv[i], "--licenses-dir") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --licenses-dir requires an argument\n");
                return 2;
            }
            licensesDir = argv[++i];
        } else if (std::strcmp(argv[i], "--allow") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --allow requires an argument\n");
                return 2;
            }
            allowedIds.push_back(argv[++i]);
        } else {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (allowedIds.empty()) {
        allowedIds.push_back("CC0-1.0");
        allowedIds.push_back("CC-BY-4.0");
    }

    // No [file] label: this tool validates a directory tree, and each message already names its own
    // path.
    reportResult(validateLicenses(dir, allowedIds, licensesDir), nullptr, exitCode);
    return exitCode;
}
