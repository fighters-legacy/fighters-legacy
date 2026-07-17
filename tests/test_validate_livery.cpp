// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_path.h"

#include <catch2/catch_test_macros.hpp>

#include "livery_validator.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fl;

namespace {

bool hasSubstr(const std::vector<std::string>& v, const std::string& needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

// A scratch content pack on disk with an aircraft entity, a livery, and its skin textures. Files whose
// only job is to EXIST (textures resolved by name) are written with placeholder bytes.
struct TempPack {
    fs::path root;

    TempPack() {
        root = fl::test::uniqueTempPath("fl-validate-livery");
        fs::create_directories(root / "liveries");
        write("manifest.toml", "[mod]\nname = \"Test Pack\"\nid = \"test-pack\"\nnamespace = \"test\"\n"
                               "version = \"0.0.1\"\nengine-api = \"1.0\"\n");
        // The aircraft the livery targets.
        write("entities/f5e.toml", "[entity]\nid = \"test:f5e\"\nname = \"F-5E\"\ncategory = \"air_vehicle\"\n"
                                   "max_hp = 100.0\n");
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

} // namespace

TEST_CASE("Single-file mode: a well-formed livery validates clean", "[livery-validator]") {
    const auto r = validateLivery(R"toml(
[livery]
name     = "Aggressor Blue"
aircraft = "fl-base:f5e"

[textures]
f5e_skin.diffuse = "f5e_aggressor_blue_diffuse"
f5e_skin.orm     = "f5e_aggressor_blue_orm"
)toml");
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty());
}

TEST_CASE("Single-file mode: parse errors come from the engine's own parser", "[livery-validator]") {
    const auto r = validateLivery("[livery]\nname = \"X\"\n"); // missing aircraft
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "aircraft"));
}

TEST_CASE("Single-file mode: plausibility warns without failing", "[livery-validator]") {
    const auto r = validateLivery(R"toml(
[livery]
name     = "Odd"
aircraft = "f5e"

[textures]
f5e_skin.metallic = "x"
)toml");
    CHECK(r.ok);                                       // legal — warnings, not errors
    CHECK(hasSubstr(r.warnings, "namespaced def id")); // aircraft is not "<ns>:<id>"
    CHECK(hasSubstr(r.warnings, "unknown map"));       // metallic is outside diffuse/normal/orm
}

TEST_CASE("Single-file mode: an empty livery warns that it is a no-op", "[livery-validator]") {
    const auto r = validateLivery("[livery]\nname = \"Factory\"\naircraft = \"fl-base:f5e\"\n");
    CHECK(r.ok);
    CHECK(hasSubstr(r.warnings, "no-op"));
}

TEST_CASE("Pack mode: a livery whose textures + aircraft resolve validates clean", "[livery-validator]") {
    TempPack pack;
    pack.write("textures/f5e_aggressor_diffuse.ktx2", "dummy");
    pack.write("textures/f5e_aggressor_orm.ktx2", "dummy");
    pack.write("liveries/aggressor.toml", "[livery]\nname = \"Aggressor\"\naircraft = \"test:f5e\"\n"
                                          "[textures]\nf5e_skin.diffuse = \"f5e_aggressor_diffuse\"\n"
                                          "f5e_skin.orm = \"f5e_aggressor_orm\"\n");
    const auto r = validateLiveryPack(pack.root.string());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("Pack mode: a missing texture asset is an error", "[livery-validator]") {
    TempPack pack;
    // diffuse texture NOT written -> unresolvable.
    pack.write("liveries/aggressor.toml", "[livery]\nname = \"Aggressor\"\naircraft = \"test:f5e\"\n"
                                          "[textures]\nf5e_skin.diffuse = \"f5e_aggressor_diffuse\"\n");
    const auto r = validateLiveryPack(pack.root.string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "f5e_aggressor_diffuse"));
}

TEST_CASE("Pack mode: an aircraft not in this pack warns but does not fail", "[livery-validator]") {
    TempPack pack;
    pack.write("textures/other_diffuse.ktx2", "dummy");
    // aircraft "fl-base:mirage" is not an entity in this pack (it would live in a base pack).
    pack.write("liveries/mirage.toml", "[livery]\nname = \"Splinter\"\naircraft = \"fl-base:mirage\"\n"
                                       "[textures]\nmirage_skin.diffuse = \"other_diffuse\"\n");
    const auto r = validateLiveryPack(pack.root.string());
    CHECK(r.ok); // warning only — the aircraft may live in a base pack
    CHECK(hasSubstr(r.warnings, "fl-base:mirage"));
}
