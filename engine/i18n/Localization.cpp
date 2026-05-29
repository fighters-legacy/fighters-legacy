// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n/Localization.h"

#include "IFilesystem.h"
#include "IFilesystemWatcher.h"
#include "ILogger.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>

// TU-static helper: scans |localeDir| for .toml files (excluding meta.toml), loads each
// into a temporary StringTable, and merges with file-stem prefix into |table|.
// If |watchedDirs| is non-null and the directory exists, the dir is appended.
static void loadLocaleDirImpl(IFilesystem& fs, ILogger& logger, const std::string& localeDir, StringTable& table,
                              std::vector<std::string>* watchedDirs) {
    auto entries = fs.scanDirectory(PathDomain::Assets, localeDir.c_str());
    if (entries.empty())
        return;
    if (watchedDirs)
        watchedDirs->push_back(localeDir);
    for (auto& entry : entries) {
        if (entry.isDirectory)
            continue;
        const std::string& name = entry.name;
        if (name.size() < 6 || name.substr(name.size() - 5) != ".toml")
            continue;
        if (name == "meta.toml")
            continue;
        std::string filePath = localeDir + "/" + name;
        std::string stem = name.substr(0, name.size() - 5);
        StringTable tmp;
        if (tmp.load(fs, logger, filePath.c_str()))
            table.mergeWithPrefix(tmp, stem);
    }
}

// ---------------------------------------------------------------------------

Localization::Localization(IFilesystem& fs, ILogger& logger) : m_fs(fs), m_logger(logger) {}

std::vector<std::string> Localization::buildLocaleChain(const std::string& lang) {
    std::vector<std::string> tags;
    std::string current = lang;
    while (true) {
        tags.push_back(current);
        auto pos = current.rfind('-');
        if (pos == std::string::npos)
            break;
        current = current.substr(0, pos);
    }
    // Ensure "en" is in the chain as the base
    if (std::find(tags.begin(), tags.end(), "en") == tags.end())
        tags.push_back("en");
    // Reverse so chain goes from least-specific to most-specific
    std::reverse(tags.begin(), tags.end());
    return tags;
}

bool Localization::readMetaRTL(const char* tag, bool& outRTL) const {
    std::string path = std::string("locale/") + tag + "/meta.toml";
    int handle = m_fs.openFile(PathDomain::Assets, path.c_str(), false);
    if (handle < 0)
        return false;
    std::size_t sz = m_fs.getFileSize(handle);
    std::string content(sz, '\0');
    if (sz > 0)
        m_fs.readFile(handle, content.data(), sz);
    m_fs.closeFile(handle);
    try {
        toml::table tbl = toml::parse(content);
        if (auto r = tbl["rtl"].value<bool>()) {
            outRTL = *r;
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool Localization::load(const char* lang, std::span<const std::string> rootDirs) {
    m_lang = lang;
    m_lastRootDirs = std::vector<std::string>(rootDirs.begin(), rootDirs.end());
    m_active = StringTable{};
    m_watchedDirs.clear();
    m_rtl = false;

    auto chain = buildLocaleChain(lang);

    for (auto& tag : chain) {
        std::string localeDir = "locale/" + tag;
        loadLocaleDir(localeDir, m_active, &m_watchedDirs);
        // Iterate roots backwards so insert_or_assign gives highest-priority root the win
        for (int i = static_cast<int>(rootDirs.size()) - 1; i >= 0; --i) {
            std::string modLocaleDir = rootDirs[i] + "/locale/" + tag;
            loadLocaleDir(modLocaleDir, m_active, &m_watchedDirs);
        }
    }

    // Read RTL from the most-specific tag that has a meta.toml
    for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
        if (readMetaRTL(chain[i].c_str(), m_rtl))
            break;
    }

    if (m_active.empty())
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     (std::string("i18n: no locale files for '") + lang + "'").c_str());
    return !m_active.empty();
}

bool Localization::reload() {
    if (m_lang.empty())
        return false;
    return load(m_lang.c_str(), m_lastRootDirs);
}

void Localization::watch(IFilesystemWatcher* watcher) {
    if (!watcher)
        return;
    for (auto& dir : m_watchedDirs)
        watcher->watch(PathDomain::Assets, dir.c_str(), false);
}

const char* Localization::get(const char* key) const {
    if (const char* v = m_active.get(key))
        return v;
    m_logger.log(LogLevel::Debug, __FILE__, __LINE__, (std::string("i18n: missing key '") + key + "'").c_str());
    return key;
}

std::string Localization::format(const char* key,
                                 std::initializer_list<std::pair<const char*, const char*>> params) const {
    const char* tmpl = get(key);
    std::string result;
    result.reserve(std::strlen(tmpl) + 32);
    const char* p = tmpl;
    while (*p) {
        if (*p == '{') {
            if (*(p + 1) == '{') {
                result += '{';
                p += 2;
            } else {
                const char* end = p + 1;
                while (*end && *end != '}')
                    ++end;
                if (!*end) {
                    // Unclosed brace — emit remainder as literal
                    result += p;
                    break;
                }
                std::string name(p + 1, end);
                bool found = false;
                for (auto& [k, v] : params) {
                    if (name == k) {
                        result += v;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    result.append(p, end + 1);
                p = end + 1;
            }
        } else if (*p == '}' && *(p + 1) == '}') {
            result += '}';
            p += 2;
        } else {
            result += *p++;
        }
    }
    return result;
}

std::string Localization::getPlural(const char* key, int n) const {
    std::string nStr = std::to_string(n);

    std::string primary, secondary;
    if (n == 0) {
        primary = std::string(key) + ".zero";
        secondary = std::string(key) + ".other";
    } else if (n == 1) {
        primary = std::string(key) + ".one";
        secondary = std::string(key) + ".other";
    } else {
        primary = std::string(key) + ".other";
        secondary = std::string(key) + ".one";
    }

    // Use m_active.get() directly to avoid spurious Debug logs for expected fallback
    if (m_active.get(primary.c_str()))
        return format(primary.c_str(), {{"n", nStr.c_str()}});
    if (m_active.get(secondary.c_str()))
        return format(secondary.c_str(), {{"n", nStr.c_str()}});

    m_logger.log(LogLevel::Debug, __FILE__, __LINE__,
                 (std::string("i18n: no plural form for '") + key + "', n=" + nStr).c_str());
    return std::string(key) + " " + nStr;
}

void Localization::loadLocaleDir(const std::string& localeDir, StringTable& table,
                                 std::vector<std::string>* watchedDirs) {
    loadLocaleDirImpl(m_fs, m_logger, localeDir, table, watchedDirs);
}

void Localization::loadOneLocale(const char* tag, std::span<const std::string> rootDirs, StringTable& out) const {
    loadLocaleDirImpl(m_fs, m_logger, std::string("locale/") + tag, out, nullptr);
    for (int i = static_cast<int>(rootDirs.size()) - 1; i >= 0; --i)
        loadLocaleDirImpl(m_fs, m_logger, rootDirs[i] + "/locale/" + tag, out, nullptr);
}

std::vector<Localization::LocaleInfo> Localization::listLocales(std::span<const std::string> rootDirs) const {
    std::set<std::string> tags;

    auto entries = m_fs.scanDirectory(PathDomain::Assets, "locale");
    for (auto& entry : entries) {
        if (entry.isDirectory)
            tags.insert(entry.name);
    }
    for (const auto& root : rootDirs) {
        std::string modLocale = root + "/locale";
        auto modEntries = m_fs.scanDirectory(PathDomain::Assets, modLocale.c_str());
        for (auto& entry : modEntries) {
            if (entry.isDirectory)
                tags.insert(entry.name);
        }
    }

    std::vector<LocaleInfo> result;
    result.reserve(tags.size());
    for (auto& tag : tags) {
        LocaleInfo info;
        info.tag = tag;
        info.displayName = tag;
        info.rtl = false;

        std::string metaPath = "locale/" + tag + "/meta.toml";
        int handle = m_fs.openFile(PathDomain::Assets, metaPath.c_str(), false);
        if (handle >= 0) {
            std::size_t sz = m_fs.getFileSize(handle);
            std::string content(sz, '\0');
            if (sz > 0)
                m_fs.readFile(handle, content.data(), sz);
            m_fs.closeFile(handle);
            try {
                toml::table tbl = toml::parse(content);
                if (auto n = tbl["name"].value<std::string>())
                    info.displayName = std::move(*n);
                if (auto r = tbl["rtl"].value<bool>())
                    info.rtl = *r;
            } catch (...) {
            }
        }
        result.push_back(std::move(info));
    }
    return result; // already sorted because std::set is ordered
}

std::vector<std::string> Localization::listMissingKeys(const char* lang, std::span<const std::string> rootDirs) const {
    if (std::string(lang) == "en")
        return {};

    StringTable enTable;
    loadOneLocale("en", rootDirs, enTable);

    StringTable langTable;
    loadOneLocale(lang, rootDirs, langTable);

    std::vector<std::string> missing;
    enTable.forEach([&](const char* key, const char*) {
        if (!langTable.get(key))
            missing.push_back(key);
    });
    std::sort(missing.begin(), missing.end());
    return missing;
}

float Localization::getCoverage(const char* lang, std::span<const std::string> rootDirs) const {
    if (std::string(lang) == "en")
        return 1.0f;

    StringTable enTable;
    loadOneLocale("en", rootDirs, enTable);

    if (enTable.empty())
        return 1.0f;

    StringTable langTable;
    loadOneLocale(lang, rootDirs, langTable);

    int total = 0;
    int present = 0;
    enTable.forEach([&](const char* key, const char*) {
        ++total;
        if (langTable.get(key))
            ++present;
    });
    return total > 0 ? static_cast<float>(present) / static_cast<float>(total) : 1.0f;
}

bool Localization::isRTL() const {
    return m_rtl;
}

const char* Localization::language() const {
    return m_lang.c_str();
}
