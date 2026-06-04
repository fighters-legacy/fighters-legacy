// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace fl {

// Normalize an IP address string for consistent comparison and storage.
// Strips IPv6 brackets and maps IPv6-mapped IPv4 (::ffff:x.x.x.x) to plain IPv4.
std::string normalizeIp(std::string_view raw);

} // namespace fl
