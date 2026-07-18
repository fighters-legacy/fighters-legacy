// SPDX-License-Identifier: GPL-3.0-or-later
#include "tex_compress.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fl;

TEST_CASE("BC7 with mipmaps produces correct toktx command", "[tex-compress]") {
    TexCompressOptions opts;
    opts.format = TexFormat::BC7;
    opts.genMipmaps = true;
    opts.toktxPath = "toktx";
    auto cmd = buildToktxCommand("in.png", "out.ktx2", opts);
    CHECK(cmd.find("--encode bc7") != std::string::npos);
    CHECK(cmd.find("--genmipmap") != std::string::npos);
    CHECK(cmd.find("--t2") != std::string::npos);
    CHECK(cmd.find("out.ktx2") != std::string::npos);
    CHECK(cmd.find("in.png") != std::string::npos);
}

TEST_CASE("BC1 without mipmaps produces correct toktx command", "[tex-compress]") {
    TexCompressOptions opts;
    opts.format = TexFormat::BC1;
    opts.genMipmaps = false;
    opts.toktxPath = "toktx";
    auto cmd = buildToktxCommand("diffuse.png", "diffuse.ktx2", opts);
    CHECK(cmd.find("--encode bc1") != std::string::npos);
    CHECK(cmd.find("--genmipmap") == std::string::npos);
    CHECK(cmd.find("--t2") != std::string::npos);
}

TEST_CASE("BC3 format produces correct toktx command", "[tex-compress]") {
    TexCompressOptions opts;
    opts.format = TexFormat::BC3;
    opts.genMipmaps = true;
    auto cmd = buildToktxCommand("canopy.png", "canopy.ktx2", opts);
    CHECK(cmd.find("--encode bc3") != std::string::npos);
}

TEST_CASE("custom toktx path appears in command", "[tex-compress]") {
    TexCompressOptions opts;
    opts.toktxPath = "/custom/path/toktx";
    auto cmd = buildToktxCommand("a.png", "a.ktx2", opts);
    CHECK(cmd.find("/custom/path/toktx") != std::string::npos);
}

TEST_CASE("path with spaces is quoted in command", "[tex-compress]") {
    TexCompressOptions opts;
    opts.toktxPath = "toktx";
    auto cmd = buildToktxCommand("my texture.png", "my texture.ktx2", opts);
    CHECK(cmd.find("\"my texture.png\"") != std::string::npos);
    CHECK(cmd.find("\"my texture.ktx2\"") != std::string::npos);
}

TEST_CASE("defaultOutputPath replaces .png extension with .ktx2", "[tex-compress]") {
    CHECK(defaultOutputPath("aircraft/fa18c_diffuse.png") == "aircraft/fa18c_diffuse.ktx2");
}

TEST_CASE("defaultOutputPath handles no parent directory", "[tex-compress]") {
    CHECK(defaultOutputPath("diffuse.png") == "diffuse.ktx2");
}

TEST_CASE("defaultOutputPath on input already ending in .ktx2 is unchanged", "[tex-compress]") {
    CHECK(defaultOutputPath("diffuse.ktx2") == "diffuse.ktx2");
}

TEST_CASE("defaultOutputPath handles double extension", "[tex-compress]") {
    // Only the last extension is replaced
    auto out = defaultOutputPath("fa18c.diffuse.png");
    CHECK(out == "fa18c.diffuse.ktx2");
}

TEST_CASE("buildToktxLayersCommand emits array mode with layer order preserved", "[tex-compress]") {
    TexCompressOptions opts;
    opts.format = TexFormat::BC7;
    opts.genMipmaps = true;
    const std::vector<std::string> inputs{"grass.png", "dirt.png", "rock.png", "snow.png"};
    const std::string cmd = buildToktxLayersCommand(inputs, "biome.ktx2", opts);
    CHECK(cmd.find("--layers 4") != std::string::npos);
    CHECK(cmd.find("--encode bc7") != std::string::npos);
    CHECK(cmd.find("--genmipmap") != std::string::npos);
    CHECK(cmd.find("--t2") != std::string::npos);
    // Output precedes the inputs, and the inputs keep their layer order (grass < dirt < rock < snow).
    const auto out = cmd.find("biome.ktx2");
    const auto grass = cmd.find("grass.png");
    const auto dirt = cmd.find("dirt.png");
    const auto rock = cmd.find("rock.png");
    const auto snow = cmd.find("snow.png");
    CHECK(out < grass);
    CHECK(grass < dirt);
    CHECK(dirt < rock);
    CHECK(rock < snow);
}

TEST_CASE("buildToktxLayersCommand quotes spaced paths and honors --no-mipmaps", "[tex-compress]") {
    TexCompressOptions opts;
    opts.format = TexFormat::BC3;
    opts.genMipmaps = false;
    const std::vector<std::string> inputs{"a b.png", "c d.png"};
    const std::string cmd = buildToktxLayersCommand(inputs, "out dir.ktx2", opts);
    CHECK(cmd.find("--layers 2") != std::string::npos);
    CHECK(cmd.find("--encode bc3") != std::string::npos);
    CHECK(cmd.find("--genmipmap") == std::string::npos);
    CHECK(cmd.find("\"a b.png\"") != std::string::npos);
    CHECK(cmd.find("\"out dir.ktx2\"") != std::string::npos);
}

TEST_CASE("compressTextureLayers rejects fewer than 2 layers", "[tex-compress]") {
    TexCompressOptions opts;
    const auto result = compressTextureLayers({"only.png"}, "out.ktx2", opts);
    CHECK_FALSE(result.ok);
    REQUIRE(!result.errors.empty());
}
