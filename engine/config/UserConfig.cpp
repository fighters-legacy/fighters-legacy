// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/UserConfig.h"

#include "IFilesystem.h"
#include "ILogger.h"

#include <toml++/toml.hpp>

#include <sstream>
#include <string>

UserConfig::UserConfig(IFilesystem& fs, ILogger& logger) : m_fs(fs), m_logger(logger) {}

bool UserConfig::load() {
    int handle = m_fs.openFile(PathDomain::UserData, kPath, false);
    if (handle < 0)
        return false;

    std::size_t size = m_fs.getFileSize(handle);
    std::string content(size, '\0');
    m_fs.readFile(handle, content.data(), size);
    m_fs.closeFile(handle);

    toml::table tbl;
    try {
        tbl = toml::parse(content);
    } catch (const toml::parse_error& e) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     (std::string("user config: failed to parse '") + kPath + "': " + e.what()).c_str());
        return false;
    }

    m_firstRunCompleted = tbl["first_run"]["completed"].value_or(false);
    return true;
}

bool UserConfig::save() {
    if (!m_fs.createDirectory(PathDomain::UserData, "config")) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "user config: failed to create config directory");
        return false;
    }

    toml::table firstRun;
    firstRun.insert_or_assign("completed", m_firstRunCompleted);
    toml::table root;
    root.insert_or_assign("first_run", std::move(firstRun));

    std::ostringstream oss;
    oss << root;
    std::string data = oss.str();

    int handle = m_fs.openFile(PathDomain::UserData, kTmpPath, true);
    if (handle < 0) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "user config: failed to open tmp file for writing");
        return false;
    }
    m_fs.writeFile(handle, data.data(), data.size());
    m_fs.closeFile(handle);

    if (!m_fs.renameFile(PathDomain::UserData, kTmpPath, kPath)) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "user config: failed to rename tmp file to final path");
        return false;
    }

    return true;
}

bool UserConfig::isFirstRunCompleted() const {
    return m_firstRunCompleted;
}

void UserConfig::setFirstRunCompleted(bool value) {
    m_firstRunCompleted = value;
}
