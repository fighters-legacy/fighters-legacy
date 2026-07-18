// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace fl {

enum class TexFormat { BC1, BC3, BC7 };

struct TexCompressOptions {
    TexFormat format{TexFormat::BC7};
    bool genMipmaps{true};
    std::string toktxPath{"toktx"};
};

struct TexCompressResult {
    bool ok{true};
    std::vector<std::string> errors;
};

// Returns the toktx command string without executing it (used by tests).
std::string buildToktxCommand(const std::string& inputPng, const std::string& outputKtx2,
                              const TexCompressOptions& opts);

// Array mode (#447): the toktx command for a 2D-array KTX2 from N layer-major input PNGs (all the
// same size). The array index is the layer id — for the biome arrays that IS the biome id
// (0 grass, 1 dirt, 2 rock, 3 snow). Emits `... --layers N out in0 in1 ... inN-1`. Not executed
// (used by tests).
std::string buildToktxLayersCommand(const std::vector<std::string>& inputPngs, const std::string& outputKtx2,
                                    const TexCompressOptions& opts);

// Returns the default output path (.ktx2) for a given input .png path.
// Uses std::filesystem::path for cross-platform correctness.
std::string defaultOutputPath(const std::string& inputPng);

// Converts inputPng to outputKtx2. Returns result.
TexCompressResult compressTexture(const std::string& inputPng, const std::string& outputKtx2,
                                  const TexCompressOptions& opts);

// Converts N layer-major PNGs into a single 2D-array KTX2 (#447). Requires >= 2 inputs.
TexCompressResult compressTextureLayers(const std::vector<std::string>& inputPngs, const std::string& outputKtx2,
                                        const TexCompressOptions& opts);

} // namespace fl
