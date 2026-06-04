// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/ConfigFile.h"
#include "ILogger.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace fl {

std::string ensureAndReadConfig(const std::string& path, std::string_view defaultContent, ILogger& log) {
    fs::path p(path);
    if (!fs::exists(p)) {
        // Write defaults then fall through to read.
        std::ofstream f(p, std::ios::binary);
        if (!f) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "ensureAndReadConfig: cannot write default to %s", path.c_str());
            log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
            return {};
        }
        f.write(defaultContent.data(), static_cast<std::streamsize>(defaultContent.size()));
    }

    std::ifstream f(p);
    if (!f) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "ensureAndReadConfig: cannot read %s", path.c_str());
        log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeConfigFile(const std::string& path, std::string_view content, ILogger& log) {
    fs::path p(path);
    fs::path tmp = p;
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
    fs::rename(tmp, p, ec);
    if (ec) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "writeConfigFile: rename failed for %s: %s", path.c_str(),
                      ec.message().c_str());
        log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace fl
