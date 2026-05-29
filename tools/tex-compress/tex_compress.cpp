// SPDX-License-Identifier: GPL-3.0-or-later
#include "tex_compress.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

std::string buildToktxCommand(const std::string& inputPng, const std::string& outputKtx2,
                              const TexCompressOptions& opts) {
    std::string cmd;

    // Quote the toktx path to handle spaces in the path (Windows SDK paths)
    cmd += "\"" + opts.toktxPath + "\"";

    switch (opts.format) {
    case TexFormat::BC1:
        cmd += " --encode bc1";
        break;
    case TexFormat::BC3:
        cmd += " --encode bc3";
        break;
    case TexFormat::BC7:
        cmd += " --encode bc7";
        break;
    }

    if (opts.genMipmaps)
        cmd += " --genmipmap";

    cmd += " --t2"; // KTX2 output format

    // Quote output and input paths to handle spaces
    cmd += " \"" + outputKtx2 + "\"";
    cmd += " \"" + inputPng + "\"";

    return cmd;
}

std::string defaultOutputPath(const std::string& inputPng) {
    fs::path p(inputPng);
    return p.replace_extension(".ktx2").string();
}

TexCompressResult compressTexture(const std::string& inputPng, const std::string& outputKtx2,
                                  const TexCompressOptions& opts) {
    TexCompressResult result;
    std::string cmd = buildToktxCommand(inputPng, outputKtx2, opts);
    int rc = std::system(cmd.c_str()); // NOLINT(cert-env33-c)
    if (rc != 0) {
        result.errors.push_back("toktx exited with code " + std::to_string(rc) +
                                " — is toktx installed? (apt install ktx-tools / brew install ktx-tools)");
        result.ok = false;
    }
    return result;
}
