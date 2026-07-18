// SPDX-License-Identifier: GPL-3.0-or-later
// Pins the shared biome-weight table (engine/render/BiomeWeights.h, #475). The terrain fragment
// shader (platform/vulkan/shaders/mesh.frag biomeWeightsForWorldCover) mirrors these exactly; if a
// value changes here, change the shader too.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/BiomeWeights.h"

using fl::biomeWeightsForWorldCover;
using fl::worldCoverIsUnclassified;
using fl::worldCoverWaterness;

namespace {
float sum(const glm::vec4& w) {
    return w.x + w.y + w.z + w.w;
}
} // namespace

TEST_CASE("biomeWeightsForWorldCover maps vegetation classes to grass", "[terrain][biome]") {
    for (uint8_t cls : {uint8_t{10}, uint8_t{20}, uint8_t{30}, uint8_t{40}, uint8_t{100}}) {
        const glm::vec4 w = biomeWeightsForWorldCover(cls);
        CHECK(w.x == Catch::Approx(1.0f)); // grass lane dominant
        CHECK(w.y == Catch::Approx(0.0f));
        CHECK(w.z == Catch::Approx(0.0f));
        CHECK(w.w == Catch::Approx(0.0f));
        CHECK_FALSE(worldCoverIsUnclassified(cls));
    }
}

TEST_CASE("biomeWeightsForWorldCover maps snow, water, bare, built-up", "[terrain][biome]") {
    CHECK(biomeWeightsForWorldCover(70).w == Catch::Approx(1.0f)); // snow/ice -> snow lane
    CHECK(worldCoverWaterness(80) == Catch::Approx(1.0f));         // open water
    CHECK(worldCoverWaterness(90) == Catch::Approx(0.5f));         // wetland = half water
    CHECK(worldCoverWaterness(30) == Catch::Approx(0.0f));         // grassland = dry
    // Bare ground and built-up are a dirt/rock mix (no grass, no snow).
    CHECK(biomeWeightsForWorldCover(60).x == Catch::Approx(0.0f));
    CHECK(biomeWeightsForWorldCover(60).w == Catch::Approx(0.0f));
    CHECK(biomeWeightsForWorldCover(50).z > 0.0f);
}

TEST_CASE("biomeWeightsForWorldCover normalizes and flags the unclassified sentinel", "[terrain][biome]") {
    // Every mapped class has positive total weight (so it normalizes without div-by-zero).
    for (uint8_t cls : {uint8_t{10}, uint8_t{50}, uint8_t{60}, uint8_t{70}, uint8_t{80}, uint8_t{90}})
        CHECK(sum(biomeWeightsForWorldCover(cls)) > 0.0f);
    // 255 (no land cover) and any unmapped code are all-zero -> shader elevation/slope fallback.
    CHECK(worldCoverIsUnclassified(255));
    CHECK(worldCoverIsUnclassified(0));
    CHECK(sum(biomeWeightsForWorldCover(255)) == Catch::Approx(0.0f));
}
