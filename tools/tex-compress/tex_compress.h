// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace fl {

// Basis Universal encodings. BOTH are transcodable — the KTX2 stores a Basis payload (vkFormat =
// VK_FORMAT_UNDEFINED), and the runtime transcodes each to BC7 on desktop / ASTC 4x4 on Apple
// Silicon / RGBA32 fallback via `ktxTexture2_TranscodeBasis` (see VkResources.cpp). That is what
// makes a content pack's textures PORTABLE across GPUs. (toktx has no raw-BCn encode: passing
// `--encode bc7` makes toktx v4 silently emit an UNCOMPRESSED texture — the latent desktop-only bug
// this replaced, #846.)
enum class TexEncoding {
    Etc1s, // ETC1S / BasisLZ: small, heavily supercompressed. For base color / albedo. Mangles
           // tangent-space data, so NEVER for normal maps.
    Uastc, // UASTC + zstd (`--zcmp`): high quality, larger. For normal / ORM / emissive, where
           // ETC1S banding is visible.
};

struct TexCompressOptions {
    TexEncoding encoding{TexEncoding::Uastc}; // safe general default; `--type diffuse` selects Etc1s.
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
