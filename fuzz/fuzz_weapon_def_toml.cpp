// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: parseWeaponDef — the TOML weapon parser (toml++) that ingests a content pack's
// weapons/*.toml. It throws std::runtime_error on any validation failure, so the harness catches
// that; the invariant is no OOB read / no UB in the parse + validation for any TOML bytes.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "weapon/WeaponDefParser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view src(reinterpret_cast<const char*>(data), size);
    try {
        (void)fl::parseWeaponDef(src);
    } catch (const std::exception&) {
        // Expected: malformed / out-of-range weapons throw. A sanitizer fault is not an exception.
    }
    return 0;
}
