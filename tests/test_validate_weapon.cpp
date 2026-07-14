// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_path.h"

#include <catch2/catch_test_macros.hpp>

#include "weapon_validator.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace fl;

namespace {

const char* kValidMissile = R"toml(
[weapon]
id       = "aim120c"
name     = "AIM-120C AMRAAM"
type     = "missile"
category = "air-to-air"

[seeker]
type            = "active-radar"
fov_deg         = 60
acquisition_nm  = 20
fire_and_forget = true

[performance]
max_range_nm      = 30
min_range_nm      = 0.5
max_speed_kts     = 2400
motor_burn_time_s = 4.5
max_g             = 30

[warhead]
blast_radius_ft = 50
damage          = 100

[load]
weight_lb   = 335
drag_factor = 0.008
)toml";

bool hasSubstr(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}

// A scratch content pack on disk, removed when the fixture goes out of scope.
struct TempPack {
    fs::path root;

    TempPack() {
        // Was keyed on `this`, which is only unique per process by grace of ASLR (#787).
        root = fl::test::uniqueTempPath("fl-validate-weapon");
        fs::create_directories(root / "weapons");
        fs::create_directories(root / "entities");
    }
    ~TempPack() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const fs::path& rel, const std::string& content) const {
        std::ofstream f(root / rel);
        f << content;
    }
};

} // namespace

TEST_CASE("A valid weapon passes cleanly", "[weapon-validator]") {
    const auto r = validateWeapon(kValidMissile);
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty()); // clean validation prints nothing — CI depends on it
}

TEST_CASE("Schema errors come from the engine's own parser", "[weapon-validator]") {
    // Unknown enum: the parser rejects it, so the validator must too — that is the whole point of
    // sharing the parser rather than reimplementing the schema.
    const std::string bad = "[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"deathray\"\ncategory=\"air-to-air\"\n"
                            "[performance]\nmax_range_nm=10\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                            "[load]\nweight_lb=100\ndrag_factor=0.01\n";
    const auto r = validateWeapon(bad);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("Malformed TOML fails rather than crashing", "[weapon-validator]") {
    const auto r = validateWeapon("[weapon] this is not toml [[[");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("Implausible-but-legal values warn without failing", "[weapon-validator]") {
    SECTION("a unit mix-up in blast radius") {
        std::string s(kValidMissile);
        s.replace(s.find("blast_radius_ft = 50"), std::string("blast_radius_ft = 50").size(),
                  "blast_radius_ft = 40000");
        const auto r = validateWeapon(s);
        CHECK(r.ok); // legal — content packs are allowed to be strange on purpose
        CHECK(hasSubstr(r.warnings, "blast_radius_ft"));
    }

    SECTION("a missile with no motor is a category slip") {
        std::string s(kValidMissile);
        s.replace(s.find("motor_burn_time_s = 4.5"), std::string("motor_burn_time_s = 4.5").size(),
                  "motor_burn_time_s = 0");
        const auto r = validateWeapon(s);
        CHECK(r.ok);
        CHECK(hasSubstr(r.warnings, "unpowered"));
    }

    SECTION("a seeker that cannot see as far as the weapon shoots") {
        std::string s(kValidMissile);
        s.replace(s.find("acquisition_nm  = 20"), std::string("acquisition_nm  = 20").size(), "acquisition_nm  = 2");
        const auto r = validateWeapon(s);
        CHECK(r.ok);
        CHECK(hasSubstr(r.warnings, "acquisition_nm"));
    }
}

TEST_CASE("Pack mode validates every weapon file", "[weapon-validator]") {
    // The hardpoint↔weapon cross-check moved to validate-entity --pack (#829) — the references
    // live in entity files. This tool's pack mode owns the WEAPONS: per-file schema +
    // plausibility, and duplicate-id detection across files.
    TempPack pack;
    pack.write("weapons/aim120c.toml", kValidMissile);

    SECTION("a valid pack passes cleanly") {
        const auto r = validatePackWeapons(pack.root.string());
        CHECK(r.ok);
        CHECK(r.errors.empty());
        CHECK(r.warnings.empty());
    }

    SECTION("a malformed weapon file is reported, not silently skipped") {
        pack.write("weapons/broken.toml", "[weapon]\nid=\"broken\"\n");
        const auto r = validatePackWeapons(pack.root.string());
        CHECK_FALSE(r.ok);
        CHECK(hasSubstr(r.errors, "broken.toml"));
    }

    SECTION("duplicate weapon ids across files are caught") {
        pack.write("weapons/copy.toml", kValidMissile); // same id
        const auto r = validatePackWeapons(pack.root.string());
        CHECK_FALSE(r.ok);
        CHECK(hasSubstr(r.errors, "duplicate weapon id"));
    }

    SECTION("plausibility warnings carry the file name") {
        std::string s(kValidMissile);
        s.replace(s.find("id       = \"aim120c\""), std::string("id       = \"aim120c\"").size(),
                  "id       = \"heavy\"");
        s.replace(s.find("weight_lb   = 335"), std::string("weight_lb   = 335").size(), "weight_lb   = 90000");
        pack.write("weapons/heavy.toml", s);
        const auto r = validatePackWeapons(pack.root.string());
        CHECK(r.ok); // implausible is legal
        CHECK(hasSubstr(r.warnings, "heavy.toml"));
    }
}

TEST_CASE("A pack with no weapons has nothing to validate", "[weapon-validator]") {
    TempPack pack;
    const auto r = validatePackWeapons(pack.root.string());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("A missing pack directory is an error", "[weapon-validator]") {
    const auto r = validatePackWeapons("/nonexistent/pack/dir");
    CHECK_FALSE(r.ok);
}
