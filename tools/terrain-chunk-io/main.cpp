// SPDX-License-Identifier: GPL-3.0-or-later
//
// terrain-chunk-io: encode/decode terrain chunks and generate procedural chunks.
//
// Subcommands:
//   decode   --input  <file.png>  --output <file.u16>
//   gen-procedural --cx <n> --cy <n> --output <file.u16> [--chunk-size-m <m>]
//
// The binary .u16 format: 4-byte magic (FLCH) + uint16 width + uint16 height
// + width*height uint16 values, row-major, little-endian.  Height encoding:
//   uint16 = clamp(elevation_m + 32768, 0, 65535)

#include "render/BuiltinGeometry.h"
#include "render/ProceduralTerrainChunk.h"
#include "render/TerrainChunkIO.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static constexpr const char* kVersion = "0.1.0";

static void printHelp() {
    std::printf("terrain-chunk-io %s\n"
                "\n"
                "Usage: terrain-chunk-io <subcommand> [options]\n"
                "\n"
                "Subcommands:\n"
                "  decode          Decode a 16-bit grayscale PNG to binary .u16 cache\n"
                "  gen-procedural  Generate a procedural terrain chunk\n"
                "\n"
                "decode options:\n"
                "  --input  <file.png>   Source 16-bit grayscale PNG\n"
                "  --output <file.u16>   Destination binary cache file\n"
                "\n"
                "gen-procedural options:\n"
                "  --cx <n>              Chunk X index (default: 0)\n"
                "  --cy <n>              Chunk Y index (default: 0)\n"
                "  --output <file.u16>   Destination binary cache file\n"
                "  --chunk-size-m <m>    Chunk physical size in metres (default: 15360)\n"
                "\n"
                "Options:\n"
                "  --help, -h      Show this help and exit\n"
                "  --version, -v   Show version and exit\n",
                kVersion);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const auto sz = f.tellg();
    if (sz <= 0)
        return {};
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return f ? buf : std::vector<uint8_t>{};
}

// ---------------------------------------------------------------------------
// Subcommand: decode
// ---------------------------------------------------------------------------

static int cmdDecode(int argc, char* argv[]) {
    std::string inputPath, outputPath;

    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            inputPath = argv[++i];
        else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            outputPath = argv[++i];
    }

    if (inputPath.empty() || outputPath.empty()) {
        std::fprintf(stderr, "error: decode requires --input and --output\n");
        return 2;
    }

    const auto raw = readFile(inputPath);
    if (raw.empty()) {
        std::fprintf(stderr, "error: cannot read '%s'\n", inputPath.c_str());
        return 1;
    }

    int w = 0, h = 0;
    auto pixels = fl::decodeTerrainChunkPng(raw.data(), raw.size(), &w, &h);
    if (pixels.empty()) {
        std::fprintf(stderr, "error: '%s' is not a valid 16-bit grayscale PNG\n", inputPath.c_str());
        return 1;
    }

    if (!fl::writeTerrainChunkCache(outputPath, pixels.data(), w, h)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", outputPath.c_str());
        return 1;
    }

    std::printf("Decoded %dx%d chunk → %s\n", w, h, outputPath.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: gen-procedural
// ---------------------------------------------------------------------------

static int cmdGenProcedural(int argc, char* argv[]) {
    int cx = 0, cy = 0;
    float chunkSizeM = 15360.f;
    std::string outputPath;

    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cx") == 0 && i + 1 < argc)
            cx = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--cy") == 0 && i + 1 < argc)
            cy = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            outputPath = argv[++i];
        else if (std::strcmp(argv[i], "--chunk-size-m") == 0 && i + 1 < argc)
            chunkSizeM = std::stof(argv[++i]);
    }

    if (outputPath.empty()) {
        std::fprintf(stderr, "error: gen-procedural requires --output\n");
        return 2;
    }

    fl::TerrainManifest manifest = fl::builtinWorldTerrainManifest();
    manifest.chunkSizeM = chunkSizeM;

    const auto pixels = fl::generateProceduralChunk(cx, cy, manifest, fl::kBuiltinProceduralParams);

    if (!fl::writeTerrainChunkCache(outputPath, pixels.data(), 513, 513)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", outputPath.c_str());
        return 1;
    }

    std::printf("Generated procedural chunk (%d, %d) → %s\n", cx, cy, outputPath.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp();
        return 2;
    }

    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        printHelp();
        return 0;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0) {
        std::printf("terrain-chunk-io %s\n", kVersion);
        return 0;
    }

    if (std::strcmp(argv[1], "decode") == 0)
        return cmdDecode(argc - 2, argv + 2);
    if (std::strcmp(argv[1], "gen-procedural") == 0)
        return cmdGenProcedural(argc - 2, argv + 2);

    std::fprintf(stderr, "error: unknown subcommand '%s'\n", argv[1]);
    printHelp();
    return 2;
}
