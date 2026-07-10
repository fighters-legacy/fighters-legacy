// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: ModLoader::load — the content-pack discovery + manifest.toml parser, including the
// path-traversal / reserved-name sanitizers applied to pack directory names. A MockFilesystem serves
// fuzz bytes as one pack's manifest under mods/<name>/. The input is split at the first newline:
// the first line becomes the pack directory-entry name (drives the name sanitizers), the remainder
// is the manifest.toml bytes. Empty assets root => no dlopen. Invariant: no OOB read / no UB.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ILogger.h"
#include "content/ModLoader.h"

#include "mock_hal.h"

namespace {
struct SilentLogger : fl::ILogger {
    void log(fl::LogLevel, const char*, int, const char*) override {}
    void setMinLevel(fl::LogLevel) override {}
    void flush() override {}
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    size_t nl = 0;
    while (nl < size && data[nl] != '\n')
        ++nl;

    std::string name(reinterpret_cast<const char*>(data), nl);
    name.erase(std::remove(name.begin(), name.end(), '\0'), name.end()); // paths are C strings
    if (name.size() > 64)
        name.resize(64);
    if (name.empty())
        name = "m";

    std::string manifest;
    if (nl + 1 < size)
        manifest.assign(reinterpret_cast<const char*>(data + nl + 1), size - nl - 1);

    SilentLogger logger;
    fl::MockFilesystem fs;
    fs.addDir("mods");
    fs.addDirEntry("mods", name, true);
    const std::string dir = "mods/" + name;
    fs.addDir(dir);
    fs.addFile(dir + "/manifest.toml", manifest);

    fl::ModLoader loader(fs, logger); // empty assets root: no native-plugin dlopen
    (void)loader.load();
    return 0;
}
