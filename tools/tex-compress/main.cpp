// SPDX-License-Identifier: GPL-3.0-or-later
#include "tex_compress.h"

#include <cstdio>
#include <cstring>
#include <string>

static constexpr const char* kVersion = "0.0.1";

static void printHelp() {
    std::printf("Usage: tex-compress [options] <input.png> [<output.ktx2>]\n"
                "\n"
                "Converts a PNG texture to KTX2 with BC block compression and mipmaps\n"
                "using the toktx tool from the Khronos KTX-Software package.\n"
                "\n"
                "Options:\n"
                "  --format bc1|bc3|bc7   Compression format (default: bc7)\n"
                "  --type diffuse|normal|orm|emissive\n"
                "                         Selects a format preset (see docs/modding/textures.md)\n"
                "                         diffuse -> bc1, normal/orm/emissive -> bc7\n"
                "                         Overridden by --format if both given\n"
                "  --no-mipmaps           Skip mipmap generation\n"
                "  --toktx <path>         Path to toktx binary (default: toktx in PATH)\n"
                "                         On Windows with spaces in path: use quotes\n"
                "  --help, -h             Show this help and exit\n"
                "  --version, -v          Show version and exit\n"
                "\n"
                "If output path is omitted, it defaults to the input path with .ktx2 extension.\n"
                "\n"
                "Exit codes:\n"
                "  0  success\n"
                "  1  conversion failure\n"
                "  2  bad arguments\n"
                "\n"
                "Prerequisites:\n"
                "  Ubuntu/Debian:  sudo apt-get install ktx-tools\n"
                "  macOS:          brew install ktx-tools\n"
                "  Windows:        included with the LunarG Vulkan SDK\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "error: no input file\n");
        printHelp();
        return 2;
    }
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        printHelp();
        return 0;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0) {
        std::printf("tex-compress %s\n", kVersion);
        return 0;
    }

    TexCompressOptions opts;
    std::string inputPng;
    std::string outputKtx2;
    bool formatSet = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--format") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --format requires an argument\n");
                return 2;
            }
            ++i;
            if (std::strcmp(argv[i], "bc1") == 0)
                opts.format = TexFormat::BC1;
            else if (std::strcmp(argv[i], "bc3") == 0)
                opts.format = TexFormat::BC3;
            else if (std::strcmp(argv[i], "bc7") == 0)
                opts.format = TexFormat::BC7;
            else {
                std::fprintf(stderr, "error: unknown format %s\n", argv[i]);
                return 2;
            }
            formatSet = true;
        } else if (std::strcmp(argv[i], "--type") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --type requires an argument\n");
                return 2;
            }
            ++i;
            if (!formatSet) {
                // Apply preset — --format overrides this if given
                if (std::strcmp(argv[i], "diffuse") == 0)
                    opts.format = TexFormat::BC1;
                else if (std::strcmp(argv[i], "normal") == 0)
                    opts.format = TexFormat::BC7;
                else if (std::strcmp(argv[i], "orm") == 0)
                    opts.format = TexFormat::BC7;
                else if (std::strcmp(argv[i], "emissive") == 0)
                    opts.format = TexFormat::BC7;
                else {
                    std::fprintf(stderr, "error: unknown type %s\n", argv[i]);
                    return 2;
                }
            }
        } else if (std::strcmp(argv[i], "--no-mipmaps") == 0) {
            opts.genMipmaps = false;
        } else if (std::strcmp(argv[i], "--toktx") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --toktx requires an argument\n");
                return 2;
            }
            opts.toktxPath = argv[++i];
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        } else if (inputPng.empty()) {
            inputPng = argv[i];
        } else if (outputKtx2.empty()) {
            outputKtx2 = argv[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }

    if (inputPng.empty()) {
        std::fprintf(stderr, "error: no input file specified\n");
        return 2;
    }
    if (outputKtx2.empty())
        outputKtx2 = defaultOutputPath(inputPng);

    auto result = compressTexture(inputPng, outputKtx2, opts);
    for (const auto& e : result.errors)
        std::fprintf(stderr, "ERROR %s\n", e.c_str());

    return result.ok ? 0 : 1;
}
