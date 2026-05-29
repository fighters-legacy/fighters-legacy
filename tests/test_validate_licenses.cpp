// SPDX-License-Identifier: GPL-3.0-or-later
#include "license_validator.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── RAII temp directory ───────────────────────────────────────────────────────

struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& suffix = "") {
        // Use a counter for cross-platform uniqueness without getpid/GetCurrentProcessId
        static std::size_t counter = 0;
        path = fs::temp_directory_path() / ("fl_lic_test_" + std::to_string(++counter) + suffix);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec); // best-effort; ignore errors on cleanup
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    void write(const std::string& rel, const std::string& content) const {
        auto p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << content;
    }
};

static const std::vector<std::string> kAllowed = {"CC0-1.0", "CC-BY-4.0"};

static const char* kValidReuseToml = "version = 1\n\n"
                                     "[[annotations]]\n"
                                     "path = \"**\"\n"
                                     "SPDX-FileCopyrightText = \"Contributors\"\n"
                                     "SPDX-License-Identifier = \"CC-BY-4.0\"\n";

static const char* kValidSidecar = "SPDX-License-Identifier: CC0-1.0\n"
                                   "SPDX-FileCopyrightText: Test Author\n";

// ── tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("empty directory with valid REUSE.toml passes", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("missing REUSE.toml fails", "[validate-licenses]") {
    TempDir d;
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("REUSE.toml") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("REUSE.toml with non-allowed identifier fails", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", "version = 1\n\n"
                          "[[annotations]]\npath = \"**\"\n"
                          "SPDX-FileCopyrightText = \"X\"\n"
                          "SPDX-License-Identifier = \"MIT\"\n");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("MIT") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("REUSE.toml without project-wide annotation fails", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", "version = 1\n\n"
                          "[[annotations]]\npath = \"specific.toml\"\n"
                          "SPDX-FileCopyrightText = \"X\"\n"
                          "SPDX-License-Identifier = \"CC-BY-4.0\"\n");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
}

TEST_CASE("valid CC0-1.0 sidecar with LICENSE cross-ref passes", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.glb.license", kValidSidecar);
    d.write("LICENSES/CC0-1.0.txt", "Creative Commons Zero");
    auto r = validateLicenses(d.path.string(), kAllowed, (d.path / "LICENSES").string());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("sidecar missing SPDX-License-Identifier fails", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.glb.license", "SPDX-FileCopyrightText: Test Author\n");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("SPDX-License-Identifier") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("sidecar missing SPDX-FileCopyrightText fails", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.glb.license", "SPDX-License-Identifier: CC0-1.0\n");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("SPDX-FileCopyrightText") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("sidecar with non-allowed identifier fails", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.glb.license", "SPDX-License-Identifier: GPL-3.0-or-later\n"
                                          "SPDX-FileCopyrightText: X\n");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("GPL") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("sidecar identifier without LICENSES file fails cross-ref check", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.glb.license", kValidSidecar);
    fs::create_directories(d.path / "LICENSES");
    // CC0-1.0.txt intentionally absent
    auto r = validateLicenses(d.path.string(), kAllowed, (d.path / "LICENSES").string());
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("CC0-1.0.txt") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("inline SPDX header with non-allowed identifier fails", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.toml", "# SPDX-License-Identifier: MIT\n"
                                   "[aircraft]\nname = \"test\"\n");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("MIT") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("inline SPDX header with allowed identifier passes", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.toml", "# SPDX-License-Identifier: CC-BY-4.0\n"
                                   "[aircraft]\nname = \"test\"\n");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("binary files without sidecars pass (skipped)", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write("aircraft/fa18c.glb", "\x47\x4c\x54\x46\x00\x00"); // GLB magic bytes
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE(".git directory is ignored", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    d.write(".git/COMMIT_EDITMSG", "arbitrary content, no license header");
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("multiple violations are all reported", "[validate-licenses]") {
    TempDir d;
    d.write("REUSE.toml", kValidReuseToml);
    // Three separate violations
    d.write("a.glb.license", "SPDX-FileCopyrightText: X\n"); // missing identifier
    d.write("b.glb.license", "SPDX-License-Identifier: MIT\n"
                             "SPDX-FileCopyrightText: X\n");               // non-allowed
    d.write("c.toml", "# SPDX-License-Identifier: Apache-2.0\nfoo = 1\n"); // non-allowed inline
    auto r = validateLicenses(d.path.string(), kAllowed, "");
    CHECK_FALSE(r.ok);
    CHECK(r.errors.size() >= 3);
}
