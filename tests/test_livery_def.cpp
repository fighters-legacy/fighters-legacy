// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "content/LiveryDef.h"

#include <stdexcept>
#include <string>

using namespace fl;

TEST_CASE("parseLiveryDef reads name, aircraft, and flattened slot.map texture overrides (#845)") {
    const LiveryDef def = parseLiveryDef(R"toml(
[livery]
name     = "Aggressor Blue"
aircraft = "fl-base:f5e"

[textures]
f5e_skin.diffuse = "f5e_aggressor_blue_diffuse"
f5e_skin.orm     = "f5e_aggressor_blue_orm"
)toml");

    CHECK(def.name == "Aggressor Blue");
    CHECK(def.aircraft == "fl-base:f5e"); // a DEF ID, never a filename (two-vocabulary rule)
    CHECK(def.textureFor("f5e_skin.diffuse") == "f5e_aggressor_blue_diffuse");
    CHECK(def.textureFor("f5e_skin.orm") == "f5e_aggressor_blue_orm");
    // An unlisted map falls back (empty) — the renderer then uses the base aircraft's texture.
    CHECK(def.textureFor("f5e_skin.normal").empty());
    CHECK(def.textureFor("f5e_canopy.diffuse").empty());
}

TEST_CASE("parseLiveryDef accepts an explicit [textures.<slot>] section form", "[livery]") {
    const LiveryDef def = parseLiveryDef(R"toml(
[livery]
name     = "Ferris"
aircraft = "fl-base:f16a"

[textures.f16_skin]
diffuse = "f16_ferris_diffuse"
)toml");
    CHECK(def.textureFor("f16_skin.diffuse") == "f16_ferris_diffuse");
}

TEST_CASE("parseLiveryDef allows an empty (no-op) livery -- degrades to base, not an error", "[livery]") {
    const LiveryDef def = parseLiveryDef(R"toml(
[livery]
name     = "Factory Fresh"
aircraft = "fl-base:f5e"
)toml");
    CHECK(def.textures.empty());
    CHECK(def.textureFor("f5e_skin.diffuse").empty());
}

TEST_CASE("parseLiveryDef rejects a missing required field", "[livery]") {
    CHECK_THROWS_AS(parseLiveryDef(R"toml(
[livery]
name = "No Aircraft"
)toml"),
                    std::runtime_error);
    CHECK_THROWS_AS(parseLiveryDef(R"toml(
[livery]
aircraft = "fl-base:f5e"
)toml"),
                    std::runtime_error);
    CHECK_THROWS_AS(parseLiveryDef("not = valid"), std::runtime_error); // no [livery] table
}

TEST_CASE("parseLiveryDef rejects a non-table [textures] slot entry", "[livery]") {
    // A bare string directly under [textures] is a map without a slot — ambiguous, so it is rejected.
    CHECK_THROWS_AS(parseLiveryDef(R"toml(
[livery]
name     = "Bad"
aircraft = "fl-base:f5e"

[textures]
diffuse = "just_a_string"
)toml"),
                    std::runtime_error);
}
