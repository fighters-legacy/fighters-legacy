// SPDX-License-Identifier: GPL-3.0-or-later
#include "license_validator.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── constants ─────────────────────────────────────────────────────────────────

static constexpr const char* kSpdxIdPrefix = "SPDX-License-Identifier:";
static constexpr const char* kSpdxCopyright = "SPDX-FileCopyrightText:";
static constexpr const char* kLicenseExt = ".license";
static constexpr const char* kReuseToml = "REUSE.toml";

static constexpr const char* kBinaryExts[] = {".glb", ".gltf", ".png", ".ogg", ".mid", ".ktx2", ".bin", ".wav"};
static constexpr std::size_t kBinaryExtsCount = sizeof(kBinaryExts) / sizeof(kBinaryExts[0]);

// ── helpers ───────────────────────────────────────────────────────────────────

static bool isAllowed(const std::string& id, const std::vector<std::string>& allowed) {
    return std::find(allowed.begin(), allowed.end(), id) != allowed.end();
}

static bool isBinaryExt(const std::string& ext) {
    for (std::size_t i = 0; i < kBinaryExtsCount; ++i)
        if (ext == kBinaryExts[i])
            return true;
    return false;
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p); // text mode: normalises CRLF on Windows
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Returns the value after "SPDX-License-Identifier: " on the first matching line, or "".
static std::string extractSpdxId(const std::string& content) {
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find(kSpdxIdPrefix);
        if (pos != std::string::npos) {
            std::string id = line.substr(pos + std::strlen(kSpdxIdPrefix));
            // trim leading/trailing whitespace
            auto s = id.find_first_not_of(" \t\r\n");
            auto e = id.find_last_not_of(" \t\r\n");
            if (s != std::string::npos)
                return id.substr(s, e - s + 1);
        }
    }
    return {};
}

static bool hasCopyrightLine(const std::string& content) {
    return content.find(kSpdxCopyright) != std::string::npos;
}

// ── REUSE.toml validation ─────────────────────────────────────────────────────

static void validateReuseToml(const fs::path& reuseTomlPath, const std::vector<std::string>& allowed,
                              LicenseValidationResult& r) {
    if (!fs::exists(reuseTomlPath)) {
        r.errors.push_back("REUSE.toml not found in root directory");
        r.ok = false;
        return;
    }

    std::string content = readFile(reuseTomlPath);
    toml::table tbl;
    try {
        tbl = toml::parse(content);
    } catch (const toml::parse_error& e) {
        r.errors.push_back(std::string("REUSE.toml: TOML parse error: ") + e.what());
        r.ok = false;
        return;
    }

    auto* annotations = tbl["annotations"].as_array();
    if (!annotations || annotations->empty()) {
        r.errors.push_back("REUSE.toml: no [[annotations]] entries found");
        r.ok = false;
        return;
    }

    // Check for a project-wide annotation covering ** and validate its identifier
    bool hasProjectWide = false;
    for (auto& el : *annotations) {
        auto* entry = el.as_table();
        if (!entry)
            continue;
        bool coversAll = false;
        auto pathNode = (*entry)["path"];
        if (pathNode.is_string()) {
            auto s = pathNode.value<std::string>();
            if (s == "**")
                coversAll = true;
        } else if (pathNode.is_array()) {
            if (auto* arr = pathNode.as_array()) {
                for (auto& p : *arr) {
                    if (p.value<std::string>() == "**") {
                        coversAll = true;
                        break;
                    }
                }
            }
        }
        if (!coversAll)
            continue;
        hasProjectWide = true;
        auto idNode = (*entry)["SPDX-License-Identifier"];
        auto id = idNode.value<std::string>();
        if (!id) {
            r.errors.push_back("REUSE.toml: project-wide annotation missing SPDX-License-Identifier");
            r.ok = false;
        } else if (!isAllowed(*id, allowed)) {
            r.errors.push_back("REUSE.toml: project-wide SPDX-License-Identifier \"" + *id +
                               "\" is not in the allowed list");
            r.ok = false;
        }
    }

    if (!hasProjectWide) {
        r.errors.push_back("REUSE.toml: no project-wide annotation (path = \"**\") found");
        r.ok = false;
    }
}

// ── main validator ────────────────────────────────────────────────────────────

LicenseValidationResult validateLicenses(const std::string& rootDir, const std::vector<std::string>& allowedSpdxIds,
                                         const std::string& licensesDir) {
    LicenseValidationResult r;

    fs::path root(rootDir);
    if (!fs::is_directory(root)) {
        r.errors.push_back("root directory does not exist: " + rootDir);
        r.ok = false;
        return r;
    }

    // Validate REUSE.toml
    validateReuseToml(root / kReuseToml, allowedSpdxIds, r);

    fs::path licDir = licensesDir.empty() ? root / "LICENSES" : fs::path(licensesDir);

    // Walk all files
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            r.warnings.push_back("filesystem error: " + ec.message());
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file())
            continue;

        fs::path p = entry.path();
        std::string rel = fs::relative(p, root).generic_string();

        // Skip .git, LICENSES, REUSE.toml
        if (rel.find(".git/") == 0 || rel == ".git")
            continue;
        if (rel.find("LICENSES/") == 0 || rel == "LICENSES")
            continue;
        if (p.filename() == kReuseToml)
            continue;

        std::string ext = p.extension().string();
        // Lowercase the extension for comparison
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Handle .license sidecar files
        if (ext == kLicenseExt) {
            std::string content = readFile(p);
            std::string id = extractSpdxId(content);
            if (id.empty()) {
                r.errors.push_back(rel + ": missing " + kSpdxIdPrefix + " line");
                r.ok = false;
            } else if (!isAllowed(id, allowedSpdxIds)) {
                r.errors.push_back(rel + ": SPDX identifier \"" + id + "\" is not allowed");
                r.ok = false;
            } else if (!licensesDir.empty()) {
                fs::path licFile = licDir / (id + ".txt");
                if (!fs::exists(licFile)) {
                    r.errors.push_back(rel + ": LICENSES/" + id + ".txt not found");
                    r.ok = false;
                }
            }
            if (!hasCopyrightLine(content)) {
                r.errors.push_back(rel + ": missing " + kSpdxCopyright + " line");
                r.ok = false;
            }
            continue;
        }

        // Skip binary extensions
        if (isBinaryExt(ext))
            continue;

        // For text files: check for inline SPDX-License-Identifier
        std::string content = readFile(p);
        if (content.find(kSpdxIdPrefix) == std::string::npos)
            continue; // no inline header — covered by REUSE.toml default
        std::string id = extractSpdxId(content);
        if (!id.empty() && !isAllowed(id, allowedSpdxIds)) {
            r.errors.push_back(rel + ": inline SPDX identifier \"" + id + "\" is not allowed");
            r.ok = false;
        }
    }

    return r;
}
