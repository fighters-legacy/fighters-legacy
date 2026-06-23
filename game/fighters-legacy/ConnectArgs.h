// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace fl {

// Parse a "--connect" argument of the form "host[:port]" or "[ipv6]:port".
// Splits on the last ':' only if the suffix is a valid 1–65535 port number.
// Otherwise the whole arg is treated as the hostname and port stays at its
// current value (caller should initialise to the desired default).
// IPv6 bracket notation "[::1]" is stripped to "::1" after port extraction.
inline bool parseConnectArg(const char* arg, std::string& host, uint16_t& port) {
    if (!arg || !*arg)
        return false;

    std::string s(arg);

    // Find last ':' — candidate port separator.
    const auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        const std::string suffix = s.substr(colon + 1);

        // Accept only all-digit, non-empty suffixes.
        bool allDigits = !suffix.empty();
        for (char c : suffix)
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }

        if (allDigits) {
            const long p = std::strtol(suffix.c_str(), nullptr, 10);
            if (p >= 1 && p <= 65535) {
                port = static_cast<uint16_t>(p);
                s = s.substr(0, colon);
            }
            // else: out-of-range — treat entire string as hostname, keep port
        }
        // else: non-numeric suffix — treat entire string as hostname, keep port
    }

    // Strip IPv6 brackets: "[::1]" → "::1".
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
        s = s.substr(1, s.size() - 2);

    host = std::move(s);
    return true;
}

} // namespace fl
