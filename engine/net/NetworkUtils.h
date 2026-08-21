// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace fl {

// Normalize an IP address string for consistent comparison and storage.
// Strips IPv6 brackets and maps IPv6-mapped IPv4 (::ffff:x.x.x.x) to plain IPv4.
std::string normalizeIp(std::string_view raw);

// Extract the normalized IP from an "ip:port" or "[ip]:port" address, as the network backends'
// getPeerAddress() returns it. The one implementation (#1243): the admission path and the admin
// commands both match peers by IP — ban, allow, kick, per-IP lockout — and two copies of this were
// two chances to disagree about which peer an operator just banned.
//
// Note the deliberate limit: an UNBRACKETED, port-less IPv6 literal truncates at its last colon,
// because that string is genuinely ambiguous. getPeerAddress() always supplies "ip:port", so no
// caller hits it; the tests pin the behaviour so a future caller finds it documented, not surprising.
std::string extractIp(std::string_view addrPort);

// Overload for the backend C strings: a null address is an empty IP, not a crash.
std::string extractIp(const char* addrPort);

} // namespace fl
