// SPDX-License-Identifier: GPL-3.0-or-later
#include "tex_compress.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

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
