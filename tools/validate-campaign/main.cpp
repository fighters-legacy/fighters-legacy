// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign_validator.h"

#include "ValidatorCli.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::printf("Usage: validate-campaign [--pack <dir>] <campaign.yaml> [more.yaml ...]\n"
                "\n"
                "Validates a campaign YAML against the engine's parseCampaign schema (#847). With\n"
                "--pack, also resolves theater manifests, story/template files, and frontline PNG\n"
                "rasters (8-bit grayscale, dimensions == the theater's frontline_grid) inside the pack.\n"
                "\n"
                "Exit codes: 0 = all valid, 1 = validation failures, 2 = bad arguments.\n"
                "  --pack <dir>   Cross-check references against the pack directory\n"
                "  --help, -h     Show this help and exit\n"
                "  --version, -v  Show version and exit\n");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    std::string packDir;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage();
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("validate-campaign %s\n", fl::kValidatorVersion);
            return 0;
        }
        if (std::strcmp(argv[i], "--pack") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--pack requires a directory\n");
                return 2;
            }
            packDir = argv[++i];
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty()) {
        printUsage();
        return 2;
    }

    bool anyFail = false;
    for (const std::string& file : files) {
        // Kept as "missing or empty", not the shared "cannot open": this tool has always treated an
        // empty campaign file as unreadable, and that message is its published output.
        const std::optional<std::string> yaml = fl::readFileToString(file.c_str());
        if (!yaml || yaml->empty()) {
            std::fprintf(stderr, "ERROR [%s] cannot read file (missing or empty)\n", file.c_str());
            anyFail = true;
            continue;
        }
        int exitCode = 0;
        fl::reportResult(packDir.empty() ? fl::validateCampaign(*yaml) : fl::validateCampaign(*yaml, packDir),
                         file.c_str(), exitCode);
        if (exitCode != 0)
            anyFail = true;
    }
    return anyFail ? 1 : 0;
}
