// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/ConfigFile.h"
#include "ILogger.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace fl {

std::string ensureAndReadConfig(const std::filesystem::path& path, std::string_view defaultContent, ILogger& log) {
    if (!fs::exists(path)) {
        // Write defaults then fall through to read.
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "ensureAndReadConfig: cannot write default to %s", path.string().c_str());
            log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
            return {};
        }
        f.write(defaultContent.data(), static_cast<std::streamsize>(defaultContent.size()));
    }

    std::ifstream f(path);
    if (!f) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "ensureAndReadConfig: cannot read %s", path.string().c_str());
        log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeConfigFile(const std::filesystem::path& path, std::string_view content, ILogger& log) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "writeConfigFile: cannot write %s", tmp.string().c_str());
            log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
            return false;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "writeConfigFile: rename failed for %s: %s", path.string().c_str(),
                      ec.message().c_str());
        log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace fl
