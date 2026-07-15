// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_path.h"

#include <catch2/catch_test_macros.hpp>

#include "entity_validator.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fl;

namespace {

bool hasSubstr(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}

// A scratch content pack on disk, shaped like fl-base-pack, removed when the fixture goes out of
// scope. Files whose only job is to EXIST (meshes, flight models resolved by name) are written with
// placeholder bytes — validate-entity checks resolution, not the referenced file's own validity;
// that is the referenced type's validator's job.
struct TempPack {
    fs::path root;

    TempPack() {
        root = fl::test::uniqueTempPath("fl-validate-entity");
        fs::create_directories(root / "entities");
        write("manifest.toml", "[mod]\nname = \"Test Pack\"\nid = \"test-pack\"\nnamespace = \"test\"\n"
                               "version = \"0.0.1\"\nengine-api = \"1.0\"\n");
    }
    ~TempPack() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const fs::path& rel, const std::string& content) const {
        fs::create_directories((root / rel).parent_path());
        std::ofstream f(root / rel);
        f << content;
    }
};

// The pre-fix fl-base-pack#24 shape: the flight model lives at aircraft/f5e/f5e.toml, so its asset
// name is "f5e/f5e" — the file wrote "f5e" and the server silently flew the builtin model instead.
std::string aircraftEntity(const std::string& id, const std::string& mesh, const std::string& flightModel,
                           const std::string& extra = {}) {
    std::string s = "[entity]\nid = \"" + id +
                    "\"\nname = \"Test Aircraft\"\ncategory = \"air_vehicle\"\n"
                    "max_hp = 100.0\n";
    if (!mesh.empty())
        s += "mesh = \"" + mesh + "\"\n";
    if (!flightModel.empty())
        s += "flight_model = \"" + flightModel + "\"\n";
    s += extra;
    return s;
}

} // namespace

TEST_CASE("Single-file mode: parse errors come from the engine's own parser", "[entity-validator]") {
    const auto r = validateEntity("[entity]\nid = \"x\"\n"); // missing name/category
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("Single-file mode: malformed TOML fails rather than crashing", "[entity-validator]") {
    const auto r = validateEntity("[entity] not toml [[[");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("Single-file mode: silent fallbacks warn without failing", "[entity-validator]") {
    const auto r = validateEntity(aircraftEntity("test:bare", "", ""));
    CHECK(r.ok);
    CHECK(hasSubstr(r.warnings, "builtin placeholder model"));
    CHECK(hasSubstr(r.warnings, "debug wedge"));
}

TEST_CASE("Pack mode: a fully-resolvable aircraft validates clean", "[entity-validator]") {
    TempPack pack;
    pack.write("aircraft/wedge.glb", "glTF-placeholder");
    pack.write("aircraft/f5e/f5e.toml", "# flight model placeholder\n");
    pack.write("entities/f5e.toml", aircraftEntity("test:f5e", "wedge", "f5e/f5e"));

    const auto r = validateEntityPack(pack.root.string());
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty()); // clean validation prints nothing — CI depends on it
}

TEST_CASE("Pack mode: an unparseable def is reported with the file name", "[entity-validator]") {
    TempPack pack;
    pack.write("entities/broken.toml", "[entity]\nid = \"test:broken\"\n"); // missing name/category
    const auto r = validateEntityPack(pack.root.string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "entities/broken.toml"));
}

TEST_CASE("Pack mode: the fl-base-pack#24 flight_model defect is an error with a correction", "[entity-validator]") {
    // The regression this tool exists for: "f5e" written where the asset name is "f5e/f5e".
    // At runtime this does not fail — it silently degrades to the builtin model.
    TempPack pack;
    pack.write("aircraft/wedge.glb", "glTF-placeholder");
    pack.write("aircraft/f5e/f5e.toml", "# flight model placeholder\n");
    pack.write("entities/f5e.toml", aircraftEntity("test:f5e", "wedge", "f5e"));

    const auto r = validateEntityPack(pack.root.string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "flight_model"));
    CHECK(hasSubstr(r.errors, "did you mean \"f5e/f5e\""));
}

TEST_CASE("Pack mode: the other wrong spelling - type directory pasted on - is also corrected", "[entity-validator]") {
    TempPack pack;
    pack.write("aircraft/wedge.glb", "glTF-placeholder");
    pack.write("aircraft/f5e/f5e.toml", "# flight model placeholder\n");
    pack.write("entities/f5e.toml", aircraftEntity("test:f5e", "wedge", "aircraft/f5e/f5e"));

    const auto r = validateEntityPack(pack.root.string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "did you mean \"f5e/f5e\""));
}

TEST_CASE("Pack mode: every asset-name reference is resolved", "[entity-validator]") {
    TempPack pack;
    pack.write("aircraft/f5e/f5e.toml", "# flight model placeholder\n");

    SECTION("unresolvable mesh") {
        pack.write("entities/e.toml", aircraftEntity("test:e", "nosuchmesh", "f5e/f5e"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK_FALSE(r.ok);
        CHECK(hasSubstr(r.errors, "mesh"));
    }

    SECTION("unresolvable ai_script") {
        pack.write("aircraft/wedge.glb", "x");
        pack.write("entities/e.toml", aircraftEntity("test:e", "wedge", "f5e/f5e", "ai_script = \"nosuchscript\"\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK_FALSE(r.ok);
        CHECK(hasSubstr(r.errors, "ai_script"));
    }

    SECTION("a resolvable ai_script passes") {
        pack.write("aircraft/wedge.glb", "x");
        pack.write("ai/fighter.lua", "-- lua\n");
        pack.write("entities/e.toml", aircraftEntity("test:e", "wedge", "f5e/f5e", "ai_script = \"fighter\"\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK(r.ok);
    }
}

TEST_CASE("Pack mode: sensor ids resolve through the id index, not the filesystem", "[entity-validator]") {
    TempPack pack;
    pack.write("aircraft/wedge.glb", "x");
    pack.write("aircraft/f5e/f5e.toml", "# fm\n");
    pack.write("sensors/radar.toml", "[sensor]\nid = \"test:radar\"\n");

    SECTION("a declared sensor id resolves") {
        pack.write("entities/e.toml", aircraftEntity("test:e", "wedge", "f5e/f5e", "sensors = [\"test:radar\"]\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK(r.ok);
    }

    SECTION("an unknown sensor id is an error") {
        pack.write("entities/e.toml", aircraftEntity("test:e", "wedge", "f5e/f5e", "sensors = [\"test:apg99\"]\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK_FALSE(r.ok);
        CHECK(hasSubstr(r.errors, "test:apg99"));
    }

    SECTION("the builtin eyeball needs no pack") {
        pack.write("entities/e.toml",
                   aircraftEntity("test:e", "wedge", "f5e/f5e", "sensors = [\"builtin:eyeball\"]\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK(r.ok);
    }
}

TEST_CASE("Pack mode: hardpoint weapon references resolve to real weapon defs", "[entity-validator]") {
    TempPack pack;
    pack.write("aircraft/wedge.glb", "x");
    pack.write("aircraft/f5e/f5e.toml", "# fm\n");
    pack.write("weapons/aim.toml", "[weapon]\nid = \"test:aim\"\n");

    const std::string station = "[[hardpoints]]\nslot = 0\ntype = \"missile\"\n";

    SECTION("a known weapon id passes") {
        pack.write("entities/e.toml", aircraftEntity("test:e", "wedge", "f5e/f5e",
                                                     station + "allowed = [\"test:aim\"]\ndefault = \"test:aim\"\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK(r.ok);
    }

    SECTION("a typo'd id in allowed is caught") {
        pack.write("entities/e.toml", aircraftEntity("test:e", "wedge", "f5e/f5e",
                                                     station + "allowed = [\"test:aim\", \"test:aim9x\"]\n"
                                                               "default = \"test:aim\"\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK_FALSE(r.ok);
        CHECK(hasSubstr(r.errors, "test:aim9x"));
    }

    SECTION("an empty station is a legitimate loadout choice (#828)") {
        pack.write("entities/e.toml", aircraftEntity("test:e", "wedge", "f5e/f5e",
                                                     station + "allowed = [\"test:aim\"]\ndefault = \"\"\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK(r.ok);
        CHECK(r.errors.empty());
    }

    SECTION("the builtin weapons need no pack") {
        pack.write("entities/e.toml",
                   aircraftEntity("test:e", "wedge", "f5e/f5e",
                                  station + "allowed = [\"builtin:ir-missile\"]\ndefault = \"builtin:ir-missile\"\n"));
        const auto r = validateEntityPack(pack.root.string());
        CHECK(r.ok);
    }
}

TEST_CASE("Pack mode: duplicate entity ids across files are caught", "[entity-validator]") {
    TempPack pack;
    pack.write("aircraft/wedge.glb", "x");
    pack.write("aircraft/f5e/f5e.toml", "# fm\n");
    pack.write("entities/a.toml", aircraftEntity("test:same", "wedge", "f5e/f5e"));
    pack.write("entities/b.toml", aircraftEntity("test:same", "wedge", "f5e/f5e"));

    const auto r = validateEntityPack(pack.root.string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "duplicate entity id"));
}

TEST_CASE("Pack mode: an id outside the pack namespace warns (the engine warns on every load)", "[entity-validator]") {
    TempPack pack;
    pack.write("aircraft/wedge.glb", "x");
    pack.write("aircraft/f5e/f5e.toml", "# fm\n");
    pack.write("entities/e.toml", aircraftEntity("otherns:e", "wedge", "f5e/f5e"));

    const auto r = validateEntityPack(pack.root.string());
    CHECK(r.ok); // a warning, not an error — the engine loads it, noisily
    CHECK(hasSubstr(r.warnings, "otherns"));
}

TEST_CASE("Pack mode: a pack with no entities has nothing to validate", "[entity-validator]") {
    TempPack pack;
    const auto r = validateEntityPack(pack.root.string());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("Pack mode: a missing pack directory is an error", "[entity-validator]") {
    const auto r = validateEntityPack("/nonexistent/pack/dir");
    CHECK_FALSE(r.ok);
}

TEST_CASE("Pack mode: a missing manifest warns but files are still validated", "[entity-validator]") {
    TempPack pack;
    fs::remove(pack.root / "manifest.toml");
    pack.write("entities/broken.toml", "[entity]\nid = \"test:broken\"\n");

    const auto r = validateEntityPack(pack.root.string());
    CHECK_FALSE(r.ok); // the broken def is still caught
    CHECK(hasSubstr(r.warnings, "manifest.toml"));
}
