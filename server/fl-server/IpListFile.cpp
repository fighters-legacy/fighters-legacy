// SPDX-License-Identifier: GPL-3.0-or-later
#include "IpListFile.h"
#include <ILogger.h>
#include <net/NetworkUtils.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace fl {

std::unordered_set<std::string> loadIpListFile(const std::string& path, ILogger* log) {
    std::unordered_set<std::string> result;
    std::ifstream f(path);
    if (!f) {
        if (log) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "loadIpListFile: cannot open %s", path.c_str());
            log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
        return result;
    }
    std::string line;
    while (std::getline(f, line)) {
        // Strip trailing '\r' so Windows-format files work on Linux/macOS.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        auto ip = fl::normalizeIp(line);
        if (!ip.empty())
            result.insert(std::move(ip));
    }
    return result;
}

void saveIpListFile(const std::string& path, const std::unordered_set<std::string>& ips, ILogger* log) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (log) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "saveIpListFile: cannot write %s", path.c_str());
            log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
        return;
    }
    for (const auto& ip : ips)
        f << ip << "\n";
}

} // namespace fl