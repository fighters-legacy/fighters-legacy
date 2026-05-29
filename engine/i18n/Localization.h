// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "i18n/StringTable.h"

#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

class IFilesystem;
class IFilesystemWatcher;
class ILogger;

// Threading:
//   load(), reload(), watch(), listLocales(), listMissingKeys(), getCoverage() — main-thread only.
//   get(), format(), getPlural(), isRTL(), language() — safe from any thread after load() completes.
class Localization {
  public:
    struct LocaleInfo {
        std::string tag;         // BCP 47 tag, e.g. "fr-CA"
        std::string displayName; // Native name from meta.toml; defaults to tag
        bool rtl = false;        // Right-to-left text direction
    };

    Localization(IFilesystem& fs, ILogger& logger);

    // Loads locale |lang| via BCP 47 fallback chain (e.g. "fr-CA" → ["en","fr","fr-CA"]).
    // Each tier loaded least-specific-first; within each tier roots loaded ascending priority.
    // Reads meta.toml from the most-specific tier available to set isRTL().
    // |rootDirs| sorted highest-priority-first (ModLoader order). Stores lang+roots for reload().
    // Returns true if at least one .toml file was loaded.
    bool load(const char* lang, std::span<const std::string> rootDirs);

    // Re-runs load() with the same lang + mods. Returns false if load() was never called.
    bool reload();

    // Registers loaded locale directories with |watcher| (null-safe).
    // Re-call after each load()/reload() to update watched paths.
    void watch(IFilesystemWatcher* watcher);

    // Returns the translated string for |key|.
    // Empty translations are skipped at load time; never returns nullptr or "".
    // If not found: logs Debug and returns |key| itself.
    const char* get(const char* key) const;

    // Returns translated string with {placeholder} tokens replaced.
    // {{ and }} produce literal { and } respectively. Unknown placeholders left as-is.
    std::string format(const char* key, std::initializer_list<std::pair<const char*, const char*>> params) const;

    // Looks up key.zero (n==0), key.one (n==1), or key.other (n>=2) and substitutes {n}.
    // Falls back to key.other if primary form is absent, then key.one.
    // Both absent: logs Debug and returns "<key> <n>".
    std::string getPlural(const char* key, int n) const;

    // Returns metadata for all available locales from locale/ and mod locale/ dirs.
    // Reads meta.toml for displayName + rtl; falls back to tag-as-name. Sorted by tag.
    std::vector<LocaleInfo> listLocales(std::span<const std::string> rootDirs) const;

    // Returns keys present in "en" but absent in |lang| (single-tier, no chain fallback).
    // Empty-string entries not counted as present. Returns empty if lang == "en". Sorted.
    std::vector<std::string> listMissingKeys(const char* lang, std::span<const std::string> rootDirs) const;

    // Returns fraction of "en" keys present in |lang| (0.0-1.0). Returns 1.0 if lang=="en"
    // or if there are no "en" keys.
    float getCoverage(const char* lang, std::span<const std::string> rootDirs) const;

    // Whether the active locale uses right-to-left text direction.
    // Loaded from meta.toml of the most-specific available tier. False before load().
    bool isRTL() const;

    // Active language tag, or "" before load().
    const char* language() const;

  private:
    void loadLocaleDir(const std::string& localeDir, StringTable& table, std::vector<std::string>* watchedDirs);
    void loadOneLocale(const char* tag, std::span<const std::string> rootDirs, StringTable& out) const;
    static std::vector<std::string> buildLocaleChain(const std::string& lang);

    // Reads locale/<tag>/meta.toml and extracts the rtl field. Returns false if not found.
    bool readMetaRTL(const char* tag, bool& outRTL) const;

    IFilesystem& m_fs;
    ILogger& m_logger;
    std::string m_lang;
    bool m_rtl = false;
    StringTable m_active;
    std::vector<std::string> m_lastRootDirs;
    std::vector<std::string> m_watchedDirs;
};
