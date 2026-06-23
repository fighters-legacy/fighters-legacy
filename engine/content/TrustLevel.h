// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Trust tier assigned to a content pack based on the presence and validity of
// its manifest signature. GPG verification is Phase 6 work; in Phase 2 the
// signature field is parsed but not cryptographically verified.
enum class TrustLevel : uint8_t {
    Unsigned = 0,   // No signature — "run at own risk"
    Community = 1,  // GPG-signed by a recognised community key (Phase 6 verification)
    Maintainer = 2, // GPG-signed by the project maintainer key (Phase 6 verification)
};

} // namespace fl
