// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crypto/Sha256.h"
#include "mod_validator.h"
#include "temp_path.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fl;
namespace fs = std::filesystem;

namespace {
void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
bool hasSubstr(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}
// A minimal pack with a valid manifest and no assets.
void writeManifest(const fs::path& root, const std::string& body) {
    writeText(root / "manifest.toml", body);
}
const char* kGoodManifest = "[mod]\nid = \"test\"\nname = \"test\"\nversion = \"1.0\"\n"
                            "engine-api = \"1.0\"\npriority = 50\nnamespace = \"test\"\n";
} // namespace

TEST_CASE("validate-mod: a minimal valid pack passes (no license check)", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    if (!r.errors.empty())
        UNSCOPED_INFO("first: " << r.errors[0]);
    CHECK(r.ok);
}

TEST_CASE("validate-mod: a missing manifest is an error", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    auto r = validateMod(dir.path().string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "no manifest.toml"));
}

TEST_CASE("validate-mod: an incompatible engine-api is an error", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), "[mod]\nid = \"t\"\nname = \"t\"\nversion = \"1.0\"\n"
                              "engine-api = \"2.0\"\npriority = 1\n");
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "engine-api"));
}

TEST_CASE("validate-mod: an unknown top-level directory warns", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    fs::create_directories(dir.path() / "junk");
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK(hasSubstr(r.warnings, "'junk'"));
}

TEST_CASE("validate-mod: [files] sha256 mismatch and match", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeText(dir.path() / "data" / "note.txt", "hello");
    const std::string good = sha256Hex(std::string("hello").data(), 5);

    // Match -> clean.
    writeManifest(dir.path(), std::string(kGoodManifest) + "\n[files]\n\"data/note.txt\" = \"" + good + "\"\n");
    ModValidateOptions o;
    o.checkLicenses = false;
    CHECK(validateMod(dir.path().string(), o).ok);

    // Mismatch -> error.
    std::string wrong = good;
    wrong[0] = (wrong[0] == 'a') ? 'b' : 'a';
    writeManifest(dir.path(), std::string(kGoodManifest) + "\n[files]\n\"data/note.txt\" = \"" + wrong + "\"\n");
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "sha256 mismatch"));

    // A [files] entry for a missing file -> error.
    writeManifest(dir.path(), std::string(kGoodManifest) + "\n[files]\n\"data/gone.txt\" = \"" + good + "\"\n");
    auto r2 = validateMod(dir.path().string(), o);
    CHECK_FALSE(r2.ok);
    CHECK(hasSubstr(r2.errors, "does not exist"));
}

TEST_CASE("validate-mod: a broken campaign in missions/ is caught with the campaigns: prefix", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    // A campaign (has story:) with a dangling next.id.
    writeText(dir.path() / "missions" / "camp.yaml",
              "name: C\nsides: [a, b]\npilot:\n  side: a\n"
              "story:\n  - id: s1\n    file: m.yaml\n    trigger: campaign_start\n"
              "    on_complete:\n      next:\n        id: ghost\n");
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "campaigns:"));
}
