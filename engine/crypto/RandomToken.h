// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace fl {

// Lowercase-hex random token drawing every nibble from std::random_device (#1233). A token is a
// capability — the single-player admin token, an MCP session id, a pilot GUID — so its
// unpredictability must match its printed length: seeding a PRNG from one 32-bit random_device
// draw caps every token it ever produces at 32 bits of entropy, no matter how long it prints.
std::string randomHexToken(std::size_t chars);

} // namespace fl
