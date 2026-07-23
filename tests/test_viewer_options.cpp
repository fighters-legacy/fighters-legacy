// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "viewer_options.h"

#include <string>
#include <vector>

using namespace fl;

namespace {
ViewerParseResult parse(std::vector<std::string> args) {
    return parseViewerOptions(args);
}
} // namespace

TEST_CASE("viewer options: defaults", "[viewer-options]") {
    auto r = parse({"--snapshot", "out.png"});
    REQUIRE(r.ok);
    CHECK(r.options.snapshotPath == "out.png");
    CHECK(r.options.width == 1280);
    CHECK(r.options.height == 720);
    CHECK(r.options.frames == 3);
    CHECK(r.options.view == PreviewDebugView::Shaded);
    CHECK_FALSE(r.options.damaged);
    CHECK_FALSE(r.options.requireContent);
}

TEST_CASE("viewer options: --size parses WxH and rejects garbage", "[viewer-options]") {
    auto ok = parse({"--size", "800x600", "--snapshot", "o.png"});
    REQUIRE(ok.ok);
    CHECK(ok.options.width == 800);
    CHECK(ok.options.height == 600);

    CHECK_FALSE(parse({"--size", "800", "--snapshot", "o.png"}).ok);
    CHECK_FALSE(parse({"--size", "0x600", "--snapshot", "o.png"}).ok);
    CHECK_FALSE(parse({"--size", "axb", "--snapshot", "o.png"}).ok);
}

TEST_CASE("viewer options: entity vs positional glb", "[viewer-options]") {
    auto e = parse({"--entity", "fl-base:f5e", "--snapshot", "o.png"});
    REQUIRE(e.ok);
    CHECK(e.options.entityId == "fl-base:f5e");
    CHECK(e.options.glbPath.empty());

    auto g = parse({"model.glb", "--snapshot", "o.png"});
    REQUIRE(g.ok);
    CHECK(g.options.glbPath == "model.glb");
    CHECK(g.options.entityId.empty());

    // Both is an error.
    CHECK_FALSE(parse({"model.glb", "--entity", "fl-base:f5e", "--snapshot", "o.png"}).ok);
    // Two positionals is an error.
    CHECK_FALSE(parse({"a.glb", "b.glb"}).ok);
}

TEST_CASE("viewer options: --view accepts the vocabulary", "[viewer-options]") {
    CHECK(parse({"--view", "shaded", "--snapshot", "o.png"}).options.view == PreviewDebugView::Shaded);
    CHECK(parse({"--view", "facecolor", "--snapshot", "o.png"}).options.view == PreviewDebugView::FaceColor);
    CHECK(parse({"--view", "wireframe", "--snapshot", "o.png"}).options.view == PreviewDebugView::Wireframe);
    CHECK(parse({"--view", "normals", "--snapshot", "o.png"}).options.view == PreviewDebugView::Normals);
    CHECK_FALSE(parse({"--view", "bogus", "--snapshot", "o.png"}).ok);
}

TEST_CASE("viewer options: --frames bounds", "[viewer-options]") {
    CHECK(parse({"--frames", "10", "--snapshot", "o.png"}).options.frames == 10);
    CHECK_FALSE(parse({"--frames", "0", "--snapshot", "o.png"}).ok);
}

TEST_CASE("viewer options: flags", "[viewer-options]") {
    auto r = parse({"--damaged", "--require-content", "--snapshot", "o.png"});
    REQUIRE(r.ok);
    CHECK(r.options.damaged);
    CHECK(r.options.requireContent);
}

TEST_CASE("viewer options: help and version short-circuit", "[viewer-options]") {
    CHECK(parse({"--help"}).options.showHelp);
    CHECK(parse({"-h"}).options.showHelp);
    CHECK(parse({"--version"}).options.showVersion);
    CHECK(parse({"-v"}).options.showVersion);
}

TEST_CASE("viewer options: unknown flag is an error", "[viewer-options]") {
    CHECK_FALSE(parse({"--nope"}).ok);
    CHECK_FALSE(parse({"--entity"}).ok); // missing value
}
