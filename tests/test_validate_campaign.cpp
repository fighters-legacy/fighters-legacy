// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "campaign/FrontlinePng.h"
#include "campaign_validator.h"
#include "temp_path.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fl;
namespace fs = std::filesystem;

namespace {
void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
void writeGrayPng(const fs::path& p, int w, int h) {
    fs::create_directories(p.parent_path());
    std::vector<uint8_t> pix(static_cast<size_t>(w) * h, 64);
    auto png = encodeFrontlinePng(pix.data(), w, h);
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}
bool hasSubstr(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}

// Build a minimal valid campaign pack; the returned string is the campaign YAML (also written to
// missions/campaign.yaml). Caller can then corrupt individual files for the failing cases.
struct Pack {
    test::TempDirGuard dir{"fl-campaign"};
    std::string campaignYaml;
    const fs::path& root() const {
        return dir.path();
    }
};

std::string buildValidPack(Pack& p, int frontlineCols = 8, int frontlineRows = 4) {
    writeText(p.root() / "manifest.toml", "[mod]\nid = \"test\"\nname = \"test\"\nnamespace = \"test\"\n");
    writeText(p.root() / "theaters" / "ukraine.toml",
              "[theater]\nid = \"ukraine\"\nname = \"Ukraine\"\n"
              "bounds = { min_lat = 44.0, min_lon = 22.0, max_lat = 52.0, max_lon = 40.0 }\n");
    writeText(p.root() / "missions" / "u01.yaml", "name: U01\nmap: ukraine\n");
    writeGrayPng(p.root() / "frontlines" / "start.png", frontlineCols, frontlineRows);
    p.campaignYaml = "name: Test Campaign\n"
                     "sides: [blue, red]\n"
                     "pilot:\n  side: blue\n"
                     "dynamic:\n"
                     "  theaters:\n"
                     "    - id: ukraine\n"
                     "      initial_frontline: frontlines/start.png\n"
                     "      frontline_grid: { cols: " +
                     std::to_string(frontlineCols) + ", rows: " + std::to_string(frontlineRows) +
                     " }\n"
                     "story:\n"
                     "  - id: intro\n    file: missions/u01.yaml\n    trigger: campaign_start\n";
    writeText(p.root() / "missions" / "campaign.yaml", p.campaignYaml);
    return p.campaignYaml;
}
} // namespace

TEST_CASE("validate-campaign: a valid pack passes clean (#847)") {
    Pack p;
    std::string yaml = buildValidPack(p);
    auto r = validateCampaign(yaml, p.root().string());
    if (!r.errors.empty())
        UNSCOPED_INFO("first error: " << r.errors[0]);
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("validate-campaign: missing frontline PNG fails with the path (#847)") {
    Pack p;
    std::string yaml = buildValidPack(p);
    fs::remove(p.root() / "frontlines" / "start.png");
    auto r = validateCampaign(yaml, p.root().string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "frontlines/start.png"));
}

TEST_CASE("validate-campaign: frontline dimensions mismatch states both shapes (#847)") {
    Pack p;
    std::string yaml = buildValidPack(p, 8, 4);
    writeGrayPng(p.root() / "frontlines" / "start.png", 10, 5); // wrong size
    auto r = validateCampaign(yaml, p.root().string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "10x5"));
    CHECK(hasSubstr(r.errors, "8x4"));
}

TEST_CASE("validate-campaign: a non-PNG frontline is rejected (#847)") {
    Pack p;
    std::string yaml = buildValidPack(p);
    writeText(p.root() / "frontlines" / "start.png", "this is not a png");
    auto r = validateCampaign(yaml, p.root().string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "frontlines/start.png"));
}

TEST_CASE("validate-campaign: a theater with no manifest fails (#847)") {
    Pack p;
    std::string yaml = buildValidPack(p);
    fs::remove(p.root() / "theaters" / "ukraine.toml");
    auto r = validateCampaign(yaml, p.root().string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "no theaters/ukraine.toml"));
}

TEST_CASE("validate-campaign: a dangling next.id fails (schema, #847)") {
    std::string yaml = "name: C\nsides: [a, b]\npilot:\n  side: a\n"
                       "story:\n"
                       "  - id: s1\n    file: m.yaml\n    trigger: campaign_start\n"
                       "    on_complete:\n      next:\n        id: nowhere\n";
    auto r = validateCampaign(yaml); // schema-only catches the dangling reference
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "unknown story 'nowhere'"));
}

TEST_CASE("validate-campaign: an unreachable story warns, does not fail (#847)") {
    std::string yaml = "name: C\nsides: [a, b]\npilot:\n  side: a\n"
                       "story:\n"
                       "  - id: s1\n    file: m.yaml\n    trigger: campaign_start\n"
                       "  - id: orphan\n    file: o.yaml\n"; // no trigger, unreachable
    auto r = validateCampaign(yaml);
    CHECK(r.ok); // a warning, not an error
    CHECK(hasSubstr(r.warnings, "orphan"));
}

TEST_CASE("classifyPackYaml tells campaigns/templates/missions apart (#847)") {
    CHECK(classifyPackYaml("name: M\nmap: x\n") == PackYamlKind::Mission);
    CHECK(classifyPackYaml("name: C\nstory:\n  - id: s1\n") == PackYamlKind::Campaign);
    CHECK(classifyPackYaml("name: C\ndynamic:\n  theaters: []\n") == PackYamlKind::Campaign);
    CHECK(classifyPackYaml("template:\n  role: cap\n") == PackYamlKind::Template);
}
