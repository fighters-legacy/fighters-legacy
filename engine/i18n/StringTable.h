// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <unordered_map>

class IFilesystem;
class ILogger;

// Threading: load/merge/mergeWithPrefix main-thread only.
// get/forEach safe from any thread after load() completes.
class StringTable {
  public:
    StringTable() = default;

    // Parses |path| (PathDomain::Assets) into flattened "section.key" entries.
    // Empty-string values are silently skipped (treated as untranslated placeholders).
    // Returns false on open failure (Warn logged) or parse error (Error logged).
    bool load(IFilesystem& fs, ILogger& logger, const char* path);

    // Merges |other| into this table; keys in |other| overwrite existing entries.
    void merge(const StringTable& other);

    // Merges |other|, prepending |prefix + "."| to every key.
    // Used by Localization to store fully-qualified keys (namespace included).
    void mergeWithPrefix(const StringTable& other, const std::string& prefix);

    // Returns value for |key|, or nullptr if absent.
    // Returned value is always non-empty (empty strings are never stored).
    // Pointer valid until the next mutating call (load/merge/mergeWithPrefix).
    const char* get(const char* key) const;

    bool empty() const {
        return m_entries.empty();
    }

    // Calls fn(key, value) for every entry. Used by getCoverage/listMissingKeys.
    template <typename Fn> void forEach(Fn&& fn) const {
        for (auto& [k, v] : m_entries)
            fn(k.c_str(), v.c_str());
    }

  private:
    std::unordered_map<std::string, std::string> m_entries;
};
