// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

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

// Returns the default output path (.ktx2) for a given input .png path.
// Uses std::filesystem::path for cross-platform correctness.
std::string defaultOutputPath(const std::string& inputPng);

// Converts inputPng to outputKtx2. Returns result.
TexCompressResult compressTexture(const std::string& inputPng, const std::string& outputKtx2,
                                  const TexCompressOptions& opts);
