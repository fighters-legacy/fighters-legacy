// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

class ILogger;

namespace fl {

// Read path. If the file does not exist, write defaultContent to it first.
// Returns file content on success, or empty string on read error (logged Warn).
std::string ensureAndReadConfig(const std::filesystem::path& path, std::string_view defaultContent, ILogger& log);

// Overwrite path with content atomically (write .tmp then rename).
// Returns false on failure (logged Warn).
bool writeConfigFile(const std::filesystem::path& path, std::string_view content, ILogger& log);

} // namespace fl
