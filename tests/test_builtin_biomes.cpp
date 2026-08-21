// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "math/Fnv.h"
#include "render/BuiltinBiomes.h"

#include <cstdint>

using namespace fl;

namespace {
// FNV-1a over a texture's pixels — a byte-stability fingerprint.
uint64_t hashPixels(const BuiltinRgbaTexture& t) {
    return fnv1a64(t.pixels.data(), t.pixels.size());
}
} // namespace

TEST_CASE("builtin biome layers: count, size, opaque", "[biomes]") {
    const auto color = builtinBiomeBaseColorLayers();
    const auto normalOrm = builtinBiomeNormalOrmLayers();
    REQUIRE(color.size() == static_cast<std::size_t>(kBiomeLayerCount));
    REQUIRE(normalOrm.size() == static_cast<std::size_t>(kBiomeLayerCount));
    for (int i = 0; i < kBiomeLayerCount; ++i) {
        CHECK(color[i].width == kBuiltinTexSize);
        CHECK(color[i].height == kBuiltinTexSize);
        CHECK(color[i].pixels.size() == static_cast<std::size_t>(kBuiltinTexSize) * kBuiltinTexSize * 4u);
        CHECK(normalOrm[i].pixels.size() == color[i].pixels.size());
        // Every layer is fully opaque (A = 255) in both maps.
        for (std::size_t p = 3; p < color[i].pixels.size(); p += 4) {
            CHECK(color[i].pixels[p] == 255);
            CHECK(normalOrm[i].pixels[p] == 255);
        }
    }
}

TEST_CASE("builtin biome layers are pairwise distinct", "[biomes]") {
    const auto color = builtinBiomeBaseColorLayers();
    for (int a = 0; a < kBiomeLayerCount; ++a)
        for (int b = a + 1; b < kBiomeLayerCount; ++b)
            CHECK(hashPixels(color[a]) != hashPixels(color[b]));
}

TEST_CASE("builtin biome layers are deterministic (byte-stable)", "[biomes]") {
    // Regenerate and compare: the generator must be pure (no rand/time).
    CHECK(hashPixels(builtinBiomeBaseColorLayers()[0]) == hashPixels(builtinBiomeBaseColorLayers()[0]));
    CHECK(hashPixels(builtinBiomeNormalOrmLayers()[2]) == hashPixels(builtinBiomeNormalOrmLayers()[2]));
    // Grass is greener than it is red/blue; snow is bright on all channels.
    const auto color = builtinBiomeBaseColorLayers();
    const auto& grass = color[static_cast<int>(BiomeLayer::Grass)];
    CHECK(grass.pixels[1] > grass.pixels[0]); // G > R at pixel 0
    CHECK(grass.pixels[1] > grass.pixels[2]); // G > B
    const auto& snow = color[static_cast<int>(BiomeLayer::Snow)];
    CHECK(snow.pixels[0] > 200);
    CHECK(snow.pixels[1] > 200);
    CHECK(snow.pixels[2] > 200);
}

TEST_CASE("builtin biome normalORM roughness differs by biome", "[biomes]") {
    const auto n = builtinBiomeNormalOrmLayers();
    // Snow (smoother) has lower base roughness than grass (rougher). Sample pixel 0's B channel.
    const auto& grass = n[static_cast<int>(BiomeLayer::Grass)];
    const auto& snow = n[static_cast<int>(BiomeLayer::Snow)];
    // Compare the mean B over the texture to avoid single-pixel noise.
    auto meanB = [](const BuiltinRgbaTexture& t) {
        uint64_t sum = 0;
        std::size_t n2 = 0;
        for (std::size_t p = 2; p < t.pixels.size(); p += 4) {
            sum += t.pixels[p];
            ++n2;
        }
        return sum / n2;
    };
    CHECK(meanB(snow) < meanB(grass));
}
