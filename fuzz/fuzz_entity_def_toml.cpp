// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: parseEntityDef — the TOML entity-definition parser (toml++) that ingests a content
// pack's entity .toml. It throws std::runtime_error on any validation failure, caught here; the
// invariant is no OOB read / no UB in the parse + validation for any attacker-controlled TOML.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "entity/EntityDefParser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view src(reinterpret_cast<const char*>(data), size);
    try {
        (void)fl::parseEntityDef(src);
    } catch (const std::exception&) {
        // Expected: malformed / invalid defs throw. A sanitizer fault is not an exception.
    }
    return 0;
}
