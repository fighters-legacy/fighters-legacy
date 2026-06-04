// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/NetworkUtils.h"

namespace fl {

std::string normalizeIp(std::string_view raw) {
    std::string_view v = raw;
    // Strip IPv6 brackets: "[2001:db8::1]" -> "2001:db8::1"
    if (!v.empty() && v.front() == '[') {
        v.remove_prefix(1);
        auto end = v.find(']');
        if (end != std::string_view::npos)
            v = v.substr(0, end);
    }
    std::string ip(v);
    // Map IPv6-mapped IPv4: "::ffff:1.2.3.4" -> "1.2.3.4"
    if (ip.size() > 7 && ip.compare(0, 7, "::ffff:") == 0)
        ip.erase(0, 7);
    return ip;
}

} // namespace fl
