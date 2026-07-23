// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign_validator.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr const char* kVersion = "0.0.1";

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

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
            std::printf("validate-campaign %s\n", kVersion);
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
        const std::string yaml = readFile(file.c_str());
        if (yaml.empty()) {
            std::fprintf(stderr, "ERROR [%s] cannot read file (missing or empty)\n", file.c_str());
            anyFail = true;
            continue;
        }
        fl::CampaignValidationResult res =
            packDir.empty() ? fl::validateCampaign(yaml) : fl::validateCampaign(yaml, packDir);
        for (const std::string& w : res.warnings)
            std::fprintf(stderr, "WARN  [%s] %s\n", file.c_str(), w.c_str());
        for (const std::string& e : res.errors)
            std::fprintf(stderr, "ERROR [%s] %s\n", file.c_str(), e.c_str());
        if (!res.ok)
            anyFail = true;
    }
    return anyFail ? 1 : 0;
}
