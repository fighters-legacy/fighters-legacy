// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystem.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// Whole-file reads over IFilesystem, in one place (#1254).
//
// Eleven sites open-size-read-close by hand, and exactly ONE of them -- LuaSandbox -- honours what
// readFile returns. IFilesystem is explicit that readFile returns the count actually read; every
// other copy passed getFileSize() and kept the buffer at that size, so a short read leaves a
// NUL-padded tail that the caller then parses as content.
//
// Latent rather than live: a physical filesystem rarely short-reads a regular file, so nothing is
// broken today. It is the kind of thing that stops being true the moment a read goes through
// something that is not a local disk.
//
// These return nullopt on open failure and never log. Callers differ deliberately on that --
// StringTable warns, ModLoader is silent by design -- so the logging stays at the call site.
//
// Header-only and stdlib-only over the HAL interface: no target, no link edge. It lives in
// engine/util rather than platform/ because every consumer is engine-side and the HAL surface
// should not grow convenience helpers (the Json.h precedent).

// The file's bytes as a string, sized to what was actually read.
[[nodiscard]] inline std::optional<std::string> readFileToString(IFilesystem& fs, PathDomain domain, const char* path) {
    const int handle = fs.openFile(domain, path, /*write=*/false);
    if (handle < 0)
        return std::nullopt;
    std::string out;
    out.resize(fs.getFileSize(handle));
    if (!out.empty())
        out.resize(fs.readFile(handle, out.data(), out.size()));
    fs.closeFile(handle);
    return out;
}

// The file's bytes, sized to what was actually read.
[[nodiscard]] inline std::optional<std::vector<uint8_t>> readFileBytes(IFilesystem& fs, PathDomain domain,
                                                                       const char* path) {
    const int handle = fs.openFile(domain, path, /*write=*/false);
    if (handle < 0)
        return std::nullopt;
    std::vector<uint8_t> out(fs.getFileSize(handle));
    if (!out.empty())
        out.resize(fs.readFile(handle, out.data(), out.size()));
    fs.closeFile(handle);
    return out;
}

} // namespace fl
