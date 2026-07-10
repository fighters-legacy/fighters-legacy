// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: parseServerConfig — the fl-server config TOML parser (toml++). Its input is an
// operator-supplied server.toml rather than a network peer, but it is a large hand-parsed surface
// with many range-validated fields; it never throws (logs a Warn + returns defaults on error), so a
// null logger is passed. Invariant: no OOB read / no UB for any attacker-controlled TOML bytes.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "server_config.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view src(reinterpret_cast<const char*>(data), size);
    (void)fl::parseServerConfig(src, nullptr);
    return 0;
}
