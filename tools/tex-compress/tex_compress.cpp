// SPDX-License-Identifier: GPL-3.0-or-later
#include "tex_compress.h"

#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace fl {

namespace {

// Append the Basis encode flags for `encoding` to a toktx argv. Both encodings imply `--t2` (the
// caller still passes it explicitly for clarity). UASTC is losslessly zstd-supercompressed via
// `--zcmp`, as the toktx manual strongly recommends — without it a UASTC KTX2 is ~4x an ETC1S one.
void appendEncodeArgs(std::vector<std::string>& argv, TexEncoding encoding) {
    argv.push_back("--encode");
    switch (encoding) {
    case TexEncoding::Etc1s:
        argv.push_back("etc1s");
        break;
    case TexEncoding::Uastc:
        argv.push_back("uastc");
        argv.push_back("--zcmp");
        break;
    }
}

// The same encode flags as a display/test string (used by the buildToktx*Command documentation
// helpers). Kept in lockstep with appendEncodeArgs by construction.
std::string encodeArgsString(TexEncoding encoding) {
    switch (encoding) {
    case TexEncoding::Etc1s:
        return " --encode etc1s";
    case TexEncoding::Uastc:
        return " --encode uastc --zcmp";
    }
    return {};
}

// Build the toktx ARGV (no shell). This is the execution path — deliberately argv, never a
// concatenated shell string, so a layer/output filename can never inject a shell command
// (CWE-78). The string builders below exist only to document + test the command shape.
std::vector<std::string> buildToktxArgv(const std::vector<std::string>& inputPngs, const std::string& outputKtx2,
                                        const TexCompressOptions& opts) {
    std::vector<std::string> argv;
    argv.push_back(opts.toktxPath);
    appendEncodeArgs(argv, opts.encoding);
    if (opts.genMipmaps)
        argv.push_back("--genmipmap");
    argv.push_back("--t2");
    if (inputPngs.size() > 1) {
        argv.push_back("--layers");
        argv.push_back(std::to_string(inputPngs.size()));
    }
    argv.push_back(outputKtx2);
    for (const std::string& in : inputPngs)
        argv.push_back(in);
    return argv;
}

// Run a program by argv and wait, returning its exit code (or -1 if it could not be spawned). No
// shell is involved on any platform, so no argument can be interpreted as a command.
int spawnAndWait(const std::vector<std::string>& argv) {
#if defined(_WIN32)
    std::vector<const char*> c;
    c.reserve(argv.size() + 1);
    for (const std::string& a : argv)
        c.push_back(a.c_str());
    c.push_back(nullptr);
    const intptr_t rc = _spawnvp(_P_WAIT, c[0], c.data());
    return rc < 0 ? -1 : static_cast<int>(rc);
#else
    std::vector<char*> c;
    c.reserve(argv.size() + 1);
    for (const std::string& a : argv)
        c.push_back(const_cast<char*>(a.c_str()));
    c.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, c[0], nullptr, nullptr, c.data(), environ) != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

std::string buildToktxCommand(const std::string& inputPng, const std::string& outputKtx2,
                              const TexCompressOptions& opts) {
    std::string cmd;

    // Quote the toktx path to handle spaces in the path (Windows SDK paths)
    cmd += "\"" + opts.toktxPath + "\"";

    cmd += encodeArgsString(opts.encoding);

    if (opts.genMipmaps)
        cmd += " --genmipmap";

    cmd += " --t2"; // KTX2 output format

    // Quote output and input paths to handle spaces
    cmd += " \"" + outputKtx2 + "\"";
    cmd += " \"" + inputPng + "\"";

    return cmd;
}

std::string buildToktxLayersCommand(const std::vector<std::string>& inputPngs, const std::string& outputKtx2,
                                    const TexCompressOptions& opts) {
    std::string cmd = "\"" + opts.toktxPath + "\"";
    cmd += encodeArgsString(opts.encoding);
    if (opts.genMipmaps)
        cmd += " --genmipmap";
    cmd += " --t2";
    cmd += " --layers " + std::to_string(inputPngs.size());
    cmd += " \"" + outputKtx2 + "\"";
    for (const std::string& in : inputPngs) // layer-major: layer index == input order
        cmd += " \"" + in + "\"";
    return cmd;
}

std::string defaultOutputPath(const std::string& inputPng) {
    fs::path p(inputPng);
    return p.replace_extension(".ktx2").string();
}

TexCompressResult compressTexture(const std::string& inputPng, const std::string& outputKtx2,
                                  const TexCompressOptions& opts) {
    TexCompressResult result;
    const int rc = spawnAndWait(buildToktxArgv({inputPng}, outputKtx2, opts));
    if (rc != 0) {
        result.errors.push_back("toktx exited with code " + std::to_string(rc) +
                                " — is toktx installed? (apt install ktx-tools / brew install ktx-tools)");
        result.ok = false;
    }
    return result;
}

TexCompressResult compressTextureLayers(const std::vector<std::string>& inputPngs, const std::string& outputKtx2,
                                        const TexCompressOptions& opts) {
    TexCompressResult result;
    if (inputPngs.size() < 2) {
        result.errors.push_back("array mode requires at least 2 layer PNGs");
        result.ok = false;
        return result;
    }
    const int rc = spawnAndWait(buildToktxArgv(inputPngs, outputKtx2, opts));
    if (rc != 0) {
        result.errors.push_back("toktx exited with code " + std::to_string(rc) +
                                " — is toktx installed (v4.3+ for --layers)? (apt install ktx-tools)");
        result.ok = false;
    }
    return result;
}

} // namespace fl
