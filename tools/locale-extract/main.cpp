// SPDX-License-Identifier: GPL-3.0-or-later
//
// locale-extract: scan C++ source for Localization key references, compare with
// locale/en/*.toml files, and optionally inject missing keys or generate
// compile-time LocaleKeys.h constants.
//
// Usage:
//   locale-extract [--src <dir>] [--locale <dir>] [--gen-keys <output>] [--dry-run]

#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Comment stripping
// ---------------------------------------------------------------------------

// Strips the C++ single-line comment (// ...) from a line, being careful not
// to truncate inside a string literal.
static std::string stripLineComment(const std::string& line) {
    bool inString = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (!inString) {
            if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
                return line.substr(0, i);
            }
            if (line[i] == '"')
                inString = true;
        } else {
            if (line[i] == '\\') {
                ++i; // skip escaped character
            } else if (line[i] == '"') {
                inString = false;
            }
        }
    }
    return line;
}

// ---------------------------------------------------------------------------
// Source scanning
// ---------------------------------------------------------------------------

// Matches .get("key"), .format("key"), .getPlural("key"),
//  ->get("key"), ->format("key"), ->getPlural("key")
static const std::regex kKeyRegex{R"re((?:\.|\->)(?:get|format|getPlural)\s*\(\s*"([^"\\]*)")re"};

// Walk |srcDir| recursively for .cpp and .h files; return all unique key strings found.
static std::set<std::string> extractKeys(const fs::path& srcDir) {
    std::set<std::string> keys;
    std::error_code ec;
    for (auto& p : fs::recursive_directory_iterator(srcDir, ec)) {
        if (!p.is_regular_file())
            continue;
        auto ext = p.path().extension().string();
        if (ext != ".cpp" && ext != ".h")
            continue;
        std::ifstream f(p.path());
        if (!f)
            continue;
        std::string line;
        while (std::getline(f, line)) {
            std::string stripped = stripLineComment(line);
            auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), kKeyRegex);
            for (auto it = begin; it != std::sregex_iterator{}; ++it)
                keys.insert((*it)[1].str());
        }
    }
    return keys;
}

// ---------------------------------------------------------------------------
// TOML key loading
// ---------------------------------------------------------------------------

// Flatten a toml table into a set of dot-separated keys (e.g. "section.key").
static void flattenKeys(const toml::table& tbl, const std::string& prefix, std::set<std::string>& out) {
    for (auto&& [k, v] : tbl) {
        std::string fullKey = prefix.empty() ? std::string(k.str()) : (prefix + "." + std::string(k.str()));
        if (v.is_table())
            flattenKeys(*v.as_table(), fullKey, out);
        else
            out.insert(fullKey); // include empty-string entries (not-yet-translated)
    }
}

// Returns all keys present in |tomlPath| (including keys with empty values).
static std::set<std::string> loadTomlKeys(const fs::path& tomlPath) {
    std::ifstream f(tomlPath);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        auto tbl = toml::parse(ss.str());
        std::set<std::string> keys;
        flattenKeys(tbl, {}, keys);
        return keys;
    } catch (...) {
        return {};
    }
}

// ---------------------------------------------------------------------------
// Comment-preserving key injection
// ---------------------------------------------------------------------------

// Inject |newSubkeys| (section → subkeys) into |tomlPath|, preserving all
// existing content and translator comments.
static bool injectKeys(const fs::path& tomlPath, const std::map<std::string, std::vector<std::string>>& newSubkeys,
                       bool dryRun) {
    std::ifstream f(tomlPath);
    if (!f) {
        // Create a new file from scratch
        if (dryRun)
            return true;
        std::ofstream out(tomlPath);
        if (!out)
            return false;
        out << "# SPDX-License-Identifier: GPL-3.0-or-later\n";
        for (auto& [section, subkeys] : newSubkeys) {
            out << "\n[" << section << "]\n";
            for (auto& sk : subkeys)
                out << sk << " = \"\"\n";
        }
        return true;
    }

    // Read all lines
    std::vector<std::string> lines;
    {
        std::string line;
        while (std::getline(f, line))
            lines.push_back(line);
        f.close();
    }

    // Track the index of the last content line per section header
    // section header format: "[section]" or "[section.subsection]"
    std::map<std::string, std::size_t> sectionEnd;
    std::string currentSection;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto& ln = lines[i];
        // Detect section header
        if (!ln.empty() && ln.front() == '[' && ln.back() == ']') {
            currentSection = ln.substr(1, ln.size() - 2);
            sectionEnd[currentSection] = i;
        } else if (!currentSection.empty()) {
            // Track last non-blank, non-comment content line as sectionEnd
            std::string trimmed = ln;
            // ltrim
            auto lp = trimmed.find_first_not_of(" \t");
            if (lp != std::string::npos && trimmed[lp] != '#')
                sectionEnd[currentSection] = i;
        }
    }

    // For each section with new keys, insert after sectionEnd[section]
    // Process in reverse order of insertion point to keep indices stable
    struct Insertion {
        std::size_t after; // insert after this line index
        std::vector<std::string> newLines;
    };
    std::vector<Insertion> insertions;

    std::vector<std::string> appendLines; // for sections not yet in file

    for (auto& [section, subkeys] : newSubkeys) {
        auto it = sectionEnd.find(section);
        if (it != sectionEnd.end()) {
            Insertion ins;
            ins.after = it->second;
            for (auto& sk : subkeys)
                ins.newLines.push_back(sk + " = \"\"");
            insertions.push_back(ins);
        } else {
            // Append new section at end of file
            appendLines.push_back("");
            appendLines.push_back("[" + section + "]");
            for (auto& sk : subkeys)
                appendLines.push_back(sk + " = \"\"");
        }
    }

    // Sort insertions by insertion point descending so indices stay valid
    std::sort(insertions.begin(), insertions.end(), [](auto& a, auto& b) { return a.after > b.after; });

    // Apply insertions in reverse order
    for (auto& ins : insertions) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(ins.after + 1), ins.newLines.begin(),
                     ins.newLines.end());
    }
    for (auto& ln : appendLines)
        lines.push_back(ln);

    if (dryRun)
        return true;

    std::ofstream out(tomlPath);
    if (!out)
        return false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size())
            out << '\n';
    }
    out << '\n';
    return true;
}

// ---------------------------------------------------------------------------
// LocaleKeys.h generation
// ---------------------------------------------------------------------------

// Sanitize a key segment into a valid C++ identifier.
static std::string sanitizeIdent(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    bool needPrefix = !s.empty() && (s[0] >= '0' && s[0] <= '9');
    if (needPrefix)
        result = "k_";
    for (char c : s) {
        if (c == '-')
            result += '_';
        else
            result += c;
    }
    return result;
}

static void generateKeysHeader(const std::set<std::string>& keys, const fs::path& output) {
    // Build a nested map: namespace → section → [keys]
    // We emit nested C++ namespaces for each segment.
    // Key format: "namespace.section.key" → namespace keys { namespace section { constexpr ... }}

    // Group keys by their dot-separated segments
    struct KeyNode {
        std::map<std::string, KeyNode> children;
        std::string fullKey; // non-empty only for leaf nodes
    };
    KeyNode root;
    for (auto& key : keys) {
        KeyNode* node = &root;
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true) {
            auto dot = key.find('.', start);
            if (dot == std::string::npos) {
                parts.push_back(key.substr(start));
                break;
            }
            parts.push_back(key.substr(start, dot - start));
            start = dot + 1;
        }
        for (std::size_t i = 0; i + 1 < parts.size(); ++i)
            node = &node->children[parts[i]];
        if (!parts.empty())
            node->children[parts.back()].fullKey = key;
    }

    fs::create_directories(output.parent_path());
    std::ofstream out(output);
    if (!out) {
        std::cerr << "error: cannot write to " << output << '\n';
        return;
    }

    out << "// SPDX-License-Identifier: GPL-3.0-or-later\n";
    out << "// Auto-generated by locale-extract. DO NOT EDIT.\n";
    out << "// Regenerate: cmake --build --preset debug --target locale-keys\n";
    out << "#pragma once\n";

    std::function<void(const KeyNode&, int)> emit = [&](const KeyNode& node, int depth) {
        std::string indent(static_cast<std::size_t>(depth * 4), ' ');
        for (auto& [name, child] : node.children) {
            std::string ident = sanitizeIdent(name);
            if (!child.fullKey.empty()) {
                out << indent << "constexpr const char* " << ident << " = \"" << child.fullKey << "\";\n";
            } else {
                out << indent << "namespace " << ident << " {\n";
                emit(child, depth + 1);
                out << indent << "}\n";
            }
        }
    };

    out << "namespace keys {\n";
    emit(root, 1);
    out << "}\n";
    std::cout << "Generated: " << output << '\n';
}

// ---------------------------------------------------------------------------
// Lint mode
// ---------------------------------------------------------------------------

static int runLint(const fs::path& srcDir, const fs::path& localeDir, bool dryRun) {
    // Group extracted keys by namespace (first segment)
    auto allKeys = extractKeys(srcDir);
    std::map<std::string, std::set<std::string>> byNamespace;
    for (auto& key : allKeys) {
        auto dot = key.find('.');
        if (dot == std::string::npos)
            continue; // keys without a namespace are ignored
        byNamespace[key.substr(0, dot)].insert(key);
    }

    int exitCode = 0;

    for (auto& [ns, nsKeys] : byNamespace) {
        fs::path tomlPath = localeDir / "en" / (ns + ".toml");

        // Strip namespace prefix to get the key as it appears in the TOML file
        std::set<std::string> extractedInFile;
        for (auto& k : nsKeys) {
            extractedInFile.insert(k.substr(ns.size() + 1));
        }

        std::set<std::string> existingInFile = loadTomlKeys(tomlPath);

        std::vector<std::string> newKeys, orphanKeys;
        for (auto& k : extractedInFile) {
            if (existingInFile.find(k) == existingInFile.end())
                newKeys.push_back(k);
        }
        for (auto& k : existingInFile) {
            if (extractedInFile.find(k) == extractedInFile.end())
                orphanKeys.push_back(k);
        }

        if (!newKeys.empty() || !orphanKeys.empty()) {
            exitCode = 1;
            std::cout << "\n[" << ns << "]\n";
            for (auto& k : newKeys)
                std::cout << "  + " << k << "  (missing from TOML)\n";
            for (auto& k : orphanKeys)
                std::cout << "  - " << k << "  (in TOML but not in source)\n";
        }

        if (!newKeys.empty() && !dryRun) {
            // Group new subkeys by section
            std::map<std::string, std::vector<std::string>> bySect;
            for (auto& k : newKeys) {
                auto dot = k.rfind('.');
                if (dot == std::string::npos) {
                    bySect[""].push_back(k);
                } else {
                    bySect[k.substr(0, dot)].push_back(k.substr(dot + 1));
                }
            }
            if (!injectKeys(tomlPath, bySect, false)) {
                std::cerr << "error: failed to write " << tomlPath << '\n';
            } else {
                std::cout << "  -> injected " << newKeys.size() << " key(s) into " << tomlPath << '\n';
            }
        }
    }

    if (exitCode == 0)
        std::cout << "locale-extract: all keys in sync.\n";
    return exitCode;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    fs::path srcDir = ".";
    fs::path localeDir = "locale";
    std::string genKeysOutput;
    bool dryRun = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--src" && i + 1 < argc) {
            srcDir = argv[++i];
        } else if (arg == "--locale" && i + 1 < argc) {
            localeDir = argv[++i];
        } else if (arg == "--gen-keys" && i + 1 < argc) {
            genKeysOutput = argv[++i];
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return 2;
        }
    }

    if (!genKeysOutput.empty()) {
        auto allKeys = extractKeys(srcDir);
        generateKeysHeader(allKeys, genKeysOutput);
        return 0;
    }

    return runLint(srcDir, localeDir, dryRun);
}
