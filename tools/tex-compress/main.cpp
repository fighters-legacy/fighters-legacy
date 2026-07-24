// SPDX-License-Identifier: GPL-3.0-or-later
#include "tex_compress.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

static constexpr const char* kVersion = "0.0.1";

static void printHelp() {
    std::printf("Usage: tex-compress [options] <input.png> [<output.ktx2>]\n"
                "\n"
                "Converts a PNG texture to a portable Basis Universal KTX2 with mipmaps, using the\n"
                "toktx tool from the Khronos KTX-Software package. The output is transcodable: the\n"
                "engine transcodes it to BC7 on desktop / ASTC on Apple Silicon at load time, so one\n"
                "committed texture runs on every GPU.\n"
                "\n"
                "Options:\n"
                "  --format etc1s|uastc   Basis encoding (default: uastc)\n"
                "                         etc1s = small, supercompressed (base color / albedo)\n"
                "                         uastc = high quality + zstd (normal / ORM / emissive)\n"
                "  --type diffuse|normal|orm|emissive\n"
                "                         Selects an encoding preset (see docs/modding/textures.md)\n"
                "                         diffuse -> etc1s, normal/orm/emissive -> uastc\n"
                "                         Overridden by --format if both given\n"
                "  --no-mipmaps           Skip mipmap generation\n"
                "  --layers <in1> <in2>...  2D-ARRAY mode: pack N layer-major PNGs into one array\n"
                "                         KTX2. Requires -o/--output. The array index IS the layer id\n"
                "                         (biome arrays: 0 grass, 1 dirt, 2 rock, 3 snow). Example:\n"
                "                         tex-compress --type orm --layers grass.png dirt.png rock.png\n"
                "                                      snow.png -o biome_normalorm.ktx2\n"
                "  -o, --output <path>    Output KTX2 path (required in --layers mode)\n"
                "  --toktx <path>         Path to toktx binary (default: toktx in PATH)\n"
                "                         On Windows with spaces in path: use quotes\n"
                "  --help, -h             Show this help and exit\n"
                "  --version, -v          Show version and exit\n"
                "\n"
                "In single-input mode, if the output path is omitted it defaults to the input path\n"
                "with a .ktx2 extension.\n"
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
    std::vector<std::string> layerInputs;
    bool layersMode = false;
    bool formatSet = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layers") == 0) {
            layersMode = true; // subsequent positionals are layer inputs
        } else if (std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires an argument\n", argv[i]);
                return 2;
            }
            outputKtx2 = argv[++i];
        } else if (std::strcmp(argv[i], "--format") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --format requires an argument\n");
                return 2;
            }
            ++i;
            if (std::strcmp(argv[i], "etc1s") == 0) {
                opts.encoding = TexEncoding::Etc1s;
            } else if (std::strcmp(argv[i], "uastc") == 0) {
                opts.encoding = TexEncoding::Uastc;
            } else if (std::strcmp(argv[i], "bc1") == 0 || std::strcmp(argv[i], "bc3") == 0) {
                // Legacy raw-BCn aliases (#846): toktx cannot emit raw BCn — it silently produced an
                // uncompressed texture. Map to the portable Basis equivalent and warn.
                opts.encoding = TexEncoding::Etc1s;
                std::fprintf(stderr,
                             "warning: --format %s is deprecated; using Basis etc1s (see docs/modding/textures.md)\n",
                             argv[i]);
            } else if (std::strcmp(argv[i], "bc7") == 0) {
                opts.encoding = TexEncoding::Uastc;
                std::fprintf(stderr,
                             "warning: --format bc7 is deprecated; using Basis uastc (see docs/modding/textures.md)\n");
            } else {
                std::fprintf(stderr, "error: unknown format %s (expected etc1s or uastc)\n", argv[i]);
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
                // Apply preset — --format overrides this if given. ETC1S mangles normals, so only
                // base color takes it; every other map keeps UASTC fidelity.
                if (std::strcmp(argv[i], "diffuse") == 0)
                    opts.encoding = TexEncoding::Etc1s;
                else if (std::strcmp(argv[i], "normal") == 0)
                    opts.encoding = TexEncoding::Uastc;
                else if (std::strcmp(argv[i], "orm") == 0)
                    opts.encoding = TexEncoding::Uastc;
                else if (std::strcmp(argv[i], "emissive") == 0)
                    opts.encoding = TexEncoding::Uastc;
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
        } else if (layersMode) {
            layerInputs.emplace_back(argv[i]); // every positional after --layers is a layer input
        } else if (inputPng.empty()) {
            inputPng = argv[i];
        } else if (outputKtx2.empty()) {
            outputKtx2 = argv[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }

    TexCompressResult result;
    if (layersMode) {
        if (layerInputs.size() < 2) {
            std::fprintf(stderr, "error: --layers mode needs at least 2 input PNGs\n");
            return 2;
        }
        if (outputKtx2.empty()) {
            std::fprintf(stderr, "error: --layers mode requires -o/--output\n");
            return 2;
        }
        result = compressTextureLayers(layerInputs, outputKtx2, opts);
    } else {
        if (inputPng.empty()) {
            std::fprintf(stderr, "error: no input file specified\n");
            return 2;
        }
        if (outputKtx2.empty())
            outputKtx2 = defaultOutputPath(inputPng);
        result = compressTexture(inputPng, outputKtx2, opts);
    }

    for (const auto& e : result.errors)
        std::fprintf(stderr, "ERROR %s\n", e.c_str());

    return result.ok ? 0 : 1;
}
