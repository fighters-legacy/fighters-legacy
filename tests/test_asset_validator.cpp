// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/AssetValidator.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Build a header span from a byte initialiser list
static std::span<const uint8_t> hdr(const std::vector<uint8_t>& v) {
    return {v.data(), std::min(v.size(), std::size_t{16})};
}

// Magic byte sequences
static const std::vector<uint8_t> kGltfBinary = {0x67, 0x6C, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00};
static const std::vector<uint8_t> kGltfJson = {'{', '"', 'a', 's', 's', 'e', 't', '"'};
static const std::vector<uint8_t> kKtx2 = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
static const std::vector<uint8_t> kPng = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
static const std::vector<uint8_t> kOgg = {0x4F, 0x67, 0x67, 0x53, 0x00, 0x02, 0x00, 0x00};
static const std::vector<uint8_t> kLuaSource = {'r', 'e', 't', 'u', 'r', 'n', ' ', '1'};
static const std::vector<uint8_t> kLuaBytecode = {0x1B, 'L', 'u', 'a', 0x54};
static const std::vector<uint8_t> kToml = {'[', 'm', 'o', 'd', ']', '\n'};
static const std::vector<uint8_t> kYaml = {'-', '-', '-', '\n'};
static const std::vector<uint8_t> kWrong = {0xDE, 0xAD, 0xBE, 0xEF};

// ---------------------------------------------------------------------------
// Mesh (glTF binary and JSON)
// ---------------------------------------------------------------------------

TEST_CASE("AssetValidator: valid glTF binary header passes") {
    AssetValidator v;
    auto r = v.validate(AssetType::Mesh, hdr(kGltfBinary), 1024);
    CHECK(r.valid);
}

TEST_CASE("AssetValidator: valid glTF JSON header passes") {
    AssetValidator v;
    auto r = v.validate(AssetType::Mesh, hdr(kGltfJson), 1024);
    CHECK(r.valid);
}

TEST_CASE("AssetValidator: wrong magic bytes for mesh asset fail") {
    AssetValidator v;
    auto r = v.validate(AssetType::Mesh, hdr(kWrong), 1024);
    CHECK_FALSE(r.valid);
    CHECK(r.reason.find("magic") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Texture (KTX2 and PNG)
// ---------------------------------------------------------------------------

TEST_CASE("AssetValidator: valid KTX2 texture header passes") {
    AssetValidator v;
    auto r = v.validate(AssetType::Texture, hdr(kKtx2), 4096);
    CHECK(r.valid);
}

TEST_CASE("AssetValidator: valid PNG texture header passes") {
    AssetValidator v;
    auto r = v.validate(AssetType::Texture, hdr(kPng), 4096);
    CHECK(r.valid);
}

TEST_CASE("AssetValidator: wrong bytes for texture fail") {
    AssetValidator v;
    auto r = v.validate(AssetType::Texture, hdr(kWrong), 4096);
    CHECK_FALSE(r.valid);
}

// ---------------------------------------------------------------------------
// Audio (OGG)
// ---------------------------------------------------------------------------

TEST_CASE("AssetValidator: valid OGG audio header passes") {
    AssetValidator v;
    auto r = v.validate(AssetType::Audio, hdr(kOgg), 65536);
    CHECK(r.valid);
}

// ---------------------------------------------------------------------------
// Lua scripts
// ---------------------------------------------------------------------------

TEST_CASE("AssetValidator: Lua source without bytecode prefix passes") {
    AssetValidator v;
    auto r = v.validate(AssetType::AIScript, hdr(kLuaSource), 512);
    CHECK(r.valid);
}

TEST_CASE("AssetValidator: Lua precompiled bytecode is rejected") {
    AssetValidator v;
    auto r = v.validate(AssetType::AIScript, hdr(kLuaBytecode), 512);
    CHECK_FALSE(r.valid);
    CHECK(r.reason.find("bytecode") != std::string::npos);
}

// ---------------------------------------------------------------------------
// TOML / YAML / Terrain (no magic, size-only)
// ---------------------------------------------------------------------------

TEST_CASE("AssetValidator: FlightModel with no magic check and valid size passes") {
    AssetValidator v;
    // Any content passes for TOML types (no magic bytes checked)
    auto r = v.validate(AssetType::FlightModel, hdr(kWrong), 1024);
    CHECK(r.valid);
}

TEST_CASE("AssetValidator: FlightModel over size limit fails") {
    AssetValidator v;
    std::size_t overLimit = 1ULL * 1024 * 1024 + 1;
    auto r = v.validate(AssetType::FlightModel, hdr(kToml), overLimit);
    CHECK_FALSE(r.valid);
    CHECK(r.reason.find("size") != std::string::npos);
}

TEST_CASE("AssetValidator: Mission YAML over size limit fails") {
    AssetValidator v;
    std::size_t overLimit = 4ULL * 1024 * 1024 + 1;
    auto r = v.validate(AssetType::Mission, hdr(kYaml), overLimit);
    CHECK_FALSE(r.valid);
}

TEST_CASE("AssetValidator: AIScript over size limit fails") {
    AssetValidator v;
    std::size_t overLimit = 512ULL * 1024 + 1;
    auto r = v.validate(AssetType::AIScript, hdr(kLuaSource), overLimit);
    CHECK_FALSE(r.valid);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_CASE("AssetValidator: empty header span is handled gracefully") {
    AssetValidator v;
    std::span<const uint8_t> empty;
    auto r = v.validate(AssetType::Mesh, empty, 0);
    CHECK_FALSE(r.valid);
    CHECK(r.reason.find("empty") != std::string::npos);
}

TEST_CASE("AssetValidator: header shorter than magic sequence is treated as mismatch") {
    AssetValidator v;
    // Only 3 bytes — KTX2 needs 12
    std::vector<uint8_t> short3 = {0xAB, 0x4B, 0x54};
    auto r = v.validate(AssetType::Texture, {short3.data(), short3.size()}, 3);
    CHECK_FALSE(r.valid);
}

TEST_CASE("AssetValidator: asset at size limit passes") {
    AssetValidator v;
    std::size_t atLimit = 50ULL * 1024 * 1024; // mesh limit
    auto r = v.validate(AssetType::Mesh, hdr(kGltfBinary), atLimit);
    CHECK(r.valid);
}

TEST_CASE("AssetValidator: asset one byte over size limit fails") {
    AssetValidator v;
    std::size_t overLimit = 50ULL * 1024 * 1024 + 1;
    auto r = v.validate(AssetType::Mesh, hdr(kGltfBinary), overLimit);
    CHECK_FALSE(r.valid);
}

TEST_CASE("AssetValidator: custom ValidationLimits override defaults") {
    ValidationLimits limits;
    limits.maxMesh = 100; // 100 bytes
    AssetValidator v(limits);

    auto pass = v.validate(AssetType::Mesh, hdr(kGltfBinary), 100);
    CHECK(pass.valid);

    auto fail = v.validate(AssetType::Mesh, hdr(kGltfBinary), 101);
    CHECK_FALSE(fail.valid);
}
