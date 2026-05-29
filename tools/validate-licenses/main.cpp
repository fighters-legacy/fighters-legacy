// SPDX-License-Identifier: GPL-3.0-or-later
#include "license_validator.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static constexpr const char* kVersion = "0.0.1";

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
    if (argc >= 2) {
        if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
            printHelp();
            return 0;
        }
        if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0) {
            std::printf("validate-licenses %s\n", kVersion);
            return 0;
        }
    }

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

    auto result = validateLicenses(dir, allowedIds, licensesDir);
    for (const auto& w : result.warnings)
        std::fprintf(stderr, "WARN  %s\n", w.c_str());
    for (const auto& e : result.errors)
        std::fprintf(stderr, "ERROR %s\n", e.c_str());

    return result.ok ? 0 : 1;
}
