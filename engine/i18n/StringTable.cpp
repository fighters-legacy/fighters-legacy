// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n/StringTable.h"

#include "IFilesystem.h"
#include "ILogger.h"

#include <toml++/toml.hpp>

#include <unordered_map>

static std::string readFileToString(IFilesystem& fs, ILogger& logger, const char* path) {
    int handle = fs.openFile(PathDomain::Assets, path, false);
    if (handle < 0) {
        logger.log(LogLevel::Warn, __FILE__, __LINE__, (std::string("i18n: cannot open file: ") + path).c_str());
        return {};
    }
    std::size_t size = fs.getFileSize(handle);
    std::string content(size, '\0');
    if (size > 0)
        fs.readFile(handle, content.data(), size);
    fs.closeFile(handle);
    return content;
}

static void flatten(const toml::table& tbl, const std::string& prefix,
                    std::unordered_map<std::string, std::string>& out) {
    for (auto&& [k, v] : tbl) {
        std::string fullKey = prefix.empty() ? std::string(k.str()) : (prefix + "." + std::string(k.str()));
        if (v.is_table()) {
            flatten(*v.as_table(), fullKey, out);
        } else if (auto s = v.value<std::string>()) {
            if (!s->empty())
                out.insert_or_assign(std::move(fullKey), std::move(*s));
        }
        // non-string, non-table leaves silently skipped
    }
}

bool StringTable::load(IFilesystem& fs, ILogger& logger, const char* path) {
    m_entries.clear();
    std::string content = readFileToString(fs, logger, path);
    if (content.empty())
        return false;
    try {
        toml::table tbl = toml::parse(content);
        flatten(tbl, {}, m_entries);
    } catch (const toml::parse_error& e) {
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   (std::string("i18n: failed to parse '") + path + "': " + e.what()).c_str());
        return false;
    }
    return true;
}

void StringTable::merge(const StringTable& other) {
    for (auto& [k, v] : other.m_entries)
        m_entries.insert_or_assign(k, v);
}

void StringTable::mergeWithPrefix(const StringTable& other, const std::string& prefix) {
    for (auto& [k, v] : other.m_entries)
        m_entries.insert_or_assign(prefix + "." + k, v);
}

const char* StringTable::get(const char* key) const {
    auto it = m_entries.find(key);
    return it != m_entries.end() ? it->second.c_str() : nullptr;
}
