// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <unordered_set>

namespace fl {

class ILogger;

// Read a one-IP-per-line file (lines beginning with '#' are comments).
// Normalises each IP via fl::normalizeIp before inserting.
// Logs Warn if the file cannot be opened; returns an empty set.
std::unordered_set<std::string> loadIpListFile(const std::string& path, ILogger* log);

// Write the set to path (one IP per line, binary mode for portable '\n').
// Logs Warn on failure.
void saveIpListFile(const std::string& path, const std::unordered_set<std::string>& ips, ILogger* log);

} // namespace fl
