// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine-campaign tests (#635): the frontline raster, the campaign-schema parser, and the deterministic
// theater-graph state machine (story injection, dynamic selection, frontline advance, save/restore).

#include "campaign/CampaignEngine.h"
#include "campaign/CampaignParser.h"
#include "campaign/CampaignRunner.h"
#include "campaign/Frontline.h"
#include "campaign/MissionTemplate.h"
#include "mission/MissionParser.h" // a generated sortie must round-trip through the real mission parser

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fl;

namespace {

// A frontline where the west half is side A (value 60) and the east half is side B (value 200).
std::vector<uint8_t> splitRaster(int cols, int rows) {
    std::vector<uint8_t> px(static_cast<std::size_t>(cols) * rows, 0);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            px[static_cast<std::size_t>(r) * cols + c] = (c < cols / 2) ? 60 : 200;
    return px;
}

// A loader that hands out a synthetic raster keyed by path, so the engine never needs a real PNG.
CampaignEngine::FrontlineLoader syntheticLoader() {
    return [](const std::string& path, Frontline& out) -> bool {
        const int cols = out.cols() > 0 ? out.cols() : 8;
        const int rows = out.rows() > 0 ? out.rows() : 4;
        Frontline f(cols, rows, out.bounds());
        std::vector<uint8_t> px;
        if (path.find("after") != std::string::npos)
            px.assign(static_cast<std::size_t>(cols) * rows, 60); // all side A after the story win
        else
            px = splitRaster(cols, rows);
        (void)f.setPixels(std::move(px));
        out = std::move(f);
        return true;
    };
}

} // namespace

// ---------------------------------------------------------------------------
// Frontline raster
// ---------------------------------------------------------------------------

TEST_CASE("decodeFrontlinePixel maps the documented value ranges", "[campaign][frontline]") {
    CHECK(decodeFrontlinePixel(0).control == FrontlineControl::Unclaimed);
    CHECK(decodeFrontlinePixel(60).control == FrontlineControl::SideA);
    CHECK(decodeFrontlinePixel(60).strength == 60);
    CHECK(decodeFrontlinePixel(127).control == FrontlineControl::SideA);
    CHECK(decodeFrontlinePixel(128).control == FrontlineControl::SideB);
    CHECK(decodeFrontlinePixel(200).control == FrontlineControl::SideB);
    CHECK(decodeFrontlinePixel(200).strength == 73); // 200 - 127
    CHECK(decodeFrontlinePixel(255).control == FrontlineControl::Contested);
}

TEST_CASE("Frontline: control queries and side fractions", "[campaign][frontline]") {
    Frontline f(8, 4, GeoBounds{});
    REQUIRE(f.setPixels(splitRaster(8, 4)));
    REQUIRE(f.valid());
    CHECK(f.at(0, 0).control == FrontlineControl::SideA);
    CHECK(f.at(7, 3).control == FrontlineControl::SideB);
    // Equal split -> each side holds half of the claimed cells.
    CHECK(f.sideFraction(0) == Catch::Approx(0.5f));
    CHECK(f.sideFraction(1) == Catch::Approx(0.5f));
    int u, a, b, c;
    f.counts(u, a, b, c);
    CHECK(a == 16);
    CHECK(b == 16);
}

TEST_CASE("Frontline: geo <-> cell mapping and world control query", "[campaign][frontline]") {
    // A 2x2 box spanning [0,1] rad lat x [0,1] rad lon (well away from the pole singularities).
    GeoBounds bounds{0.0, 0.0, 1.0, 1.0};
    Frontline f(2, 2, bounds);
    // NW=A, NE=B, SW=A, SE=B (west column A, east column B)
    REQUIRE(f.setPixels({60, 200, 60, 200}));
    int col = -1, row = -1;
    // A point near the NW corner (high lat, low lon) is col 0 row 0.
    REQUIRE(f.geoToCell(0.9, 0.1, col, row));
    CHECK(col == 0);
    CHECK(row == 0);
    CHECK(f.at(col, row).control == FrontlineControl::SideA);
    // A point near the SE corner (low lat, high lon) is col 1 row 1.
    REQUIRE(f.geoToCell(0.1, 0.9, col, row));
    CHECK(col == 1);
    CHECK(row == 1);
    CHECK(f.at(col, row).control == FrontlineControl::SideB);
    // Outside the bounds -> no cell.
    CHECK_FALSE(f.geoToCell(2.0, 0.5, col, row));
}

TEST_CASE("Frontline::territoryAtWorld maps control to the ejection landing zone (#672)", "[campaign][frontline]") {
    // West column side A, east column side B, over a small equatorial box.
    GeoBounds bounds{0.0, 0.0, 0.2, 0.2};
    Frontline f(2, 1, bounds);
    REQUIRE(f.setPixels({60, 200})); // col 0 = A, col 1 = B
    // A world position over the western (side-A) cell.
    double wx = 0.0, wy = 0.0, wz = 0.0;
    double latW = 0.0, lonW = 0.0;
    f.cellCenterLatLon(0, 0, latW, lonW);
    geodeticToWorld(LatLonAlt{latW, lonW, 0.0}, wx, wy, wz, kEarthRadiusM);
    // Pilot side A (index 0): over its own ground -> friendly (Rescued); the enemy pilot -> hostile.
    CHECK(f.territoryAtWorld(wx, wy, wz, /*pilotSideIndex=*/0) == TerritoryControl::Friendly);
    CHECK(f.territoryAtWorld(wx, wy, wz, /*pilotSideIndex=*/1) == TerritoryControl::Hostile);
    // Off-map -> neutral (MIA).
    CHECK(f.territoryAtWorld(1e12, 0.0, 0.0, 0) == TerritoryControl::Neutral);
}

// ---------------------------------------------------------------------------
// Campaign parser
// ---------------------------------------------------------------------------

const char* kCampaignYaml = R"yaml(
name: "Forgotten Skies"
version: "1.0"
sides: [nato, russia]
pilot:
  side: nato
  rank_table: ranks/nato_ranks.toml
  persistent_stats: true
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/ukraine_start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units:
        nato:   { armor: 40, infantry: 60 }
        russia: { armor: 55, sam: 4 }
      templates:
        - { role: intercept, file: templates/intercept.yaml, weight: 3 }
        - { role: sead,      file: templates/sead.yaml,      weight: 1, requires: enemy_sam }
story:
  - id: u01_storm
    file: missions/u01.yaml
    label: "Storm Warning"
    trigger: campaign_start
    locks_dynamic: true
    theater: ukraine
    on_complete:
      set_frontline: frontlines/ukraine_after_u01.png
      unlock: ukraine
      next: { after_sorties: 3, id: u02_iron }
  - id: u02_iron
    file: missions/u02.yaml
    label: "Iron Fist"
)yaml";

TEST_CASE("parseCampaign populates the model", "[campaign][parser]") {
    auto r = parseCampaign(kCampaignYaml);
    for (const auto& e : r.errors)
        UNSCOPED_INFO("parse error: " << e);
    REQUIRE(r.ok);
    const CampaignDef& c = r.campaign;
    CHECK(c.name == "Forgotten Skies");
    CHECK(c.sides[0] == "nato");
    CHECK(c.sides[1] == "russia");
    CHECK(c.pilotSide == "nato");
    CHECK(c.persistentStats);
    CHECK(c.dynamicEnabled);
    REQUIRE(c.theaters.size() == 1);
    CHECK(c.theaters[0].id == "ukraine");
    CHECK(c.theaters[0].frontlineCols == 8);
    CHECK(c.theaters[0].groundUnits.at("russia").at("sam") == 4);
    REQUIRE(c.theaters[0].templates.size() == 2);
    CHECK(c.theaters[0].templates[1].requiresTag == "enemy_sam");
    REQUIRE(c.story.size() == 2);
    CHECK(c.story[0].trigger == "campaign_start");
    CHECK(c.story[0].locksDynamic);
    CHECK(c.story[0].onComplete.unlock == "ukraine");
    CHECK(c.story[0].onComplete.nextId == "u02_iron");
    CHECK(c.story[0].onComplete.nextTrigger == "after_sorties:3");
}

TEST_CASE("parseCampaign rejects a wrong-arity sides list and an unknown pilot side", "[campaign][parser]") {
    auto r1 = parseCampaign("name: x\nsides: [only_one]\nstory:\n  - { id: a, file: a.yaml }\n");
    CHECK_FALSE(r1.ok);
    auto r2 = parseCampaign("name: x\nsides: [a, b]\npilot: { side: c }\nstory:\n  - { id: m, file: m.yaml }\n");
    CHECK_FALSE(r2.ok);
}

// ---------------------------------------------------------------------------
// Campaign engine — story injection, dynamic selection, frontline advance
// ---------------------------------------------------------------------------

TEST_CASE("CampaignEngine: a campaign_start story mission is injected first and locks dynamic (#635)",
          "[campaign][engine]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    CampaignEngine eng(parsed.campaign, 12345, syntheticLoader());

    // ukraine is unlocked only by u01's on_complete, so it starts LOCKED — the story is the only thing
    // flyable, and dynamic is frozen.
    CHECK_FALSE(eng.theaterUnlocked("ukraine"));
    CHECK(eng.dynamicLocked());

    NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Story);
    CHECK(nm.missionId == "u01_storm");
    CHECK(nm.missionFile == "missions/u01.yaml");
}

TEST_CASE("CampaignEngine: completing the story advances the frontline and unlocks the theater (#635)",
          "[campaign][engine]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    CampaignEngine eng(parsed.campaign, 1, syntheticLoader());

    // Before: split control (0.5 each).
    CHECK(eng.frontlineFraction("ukraine", 0) == Catch::Approx(0.5f));

    eng.recordOutcome("u01_storm", /*success=*/true);

    // After: set_frontline replaced the raster (all side A), theater unlocked, lock lifted.
    CHECK(eng.theaterUnlocked("ukraine"));
    CHECK_FALSE(eng.dynamicLocked());
    CHECK(eng.frontlineFraction("ukraine", 0) == Catch::Approx(1.0f)); // "after" raster is all A
    CHECK(eng.completedStory().size() == 1);
}

TEST_CASE("CampaignEngine: after the story, dynamic sorties generate; u02 injects after 3 sorties (#635)",
          "[campaign][engine]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    CampaignEngine eng(parsed.campaign, 42, syntheticLoader());
    eng.recordOutcome("u01_storm", true); // unlock + arm u02 after 3 sorties

    // Now a dynamic sortie is generated (a real mission file from a template).
    NextMission d0 = eng.nextMission();
    REQUIRE(d0.kind == NextMission::Kind::Dynamic);
    CHECK(d0.theaterId == "ukraine");
    CHECK_FALSE(d0.missionFile.empty());
    CHECK(d0.opforCount == 59); // russia armor 55 + sam 4

    // Fly three dynamic sorties; the 3rd arms u02.
    eng.recordOutcome(d0.missionId, true);
    eng.recordOutcome(eng.nextMission().missionId, true);
    eng.recordOutcome(eng.nextMission().missionId, true);
    CHECK(eng.sortiesFlown() == 3);

    NextMission after = eng.nextMission();
    REQUIRE(after.kind == NextMission::Kind::Story);
    CHECK(after.missionId == "u02_iron"); // injected at after_sorties:3
}

TEST_CASE("CampaignEngine: weighted dynamic selection is deterministic under a seed (#635)", "[campaign][engine]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    auto run = [&](uint64_t seed) {
        CampaignEngine eng(parsed.campaign, seed, syntheticLoader());
        eng.recordOutcome("u01_storm", true);
        std::vector<std::string> roles;
        for (int i = 0; i < 6; ++i) {
            NextMission nm = eng.nextMission();
            if (nm.kind != NextMission::Kind::Dynamic)
                break;
            roles.push_back(nm.role);
            eng.recordOutcome(nm.missionId, true);
        }
        return roles;
    };
    CHECK(run(777) == run(777)); // same seed -> same sequence (replay-deterministic)
    // The 3:1 intercept:sead weighting means intercept dominates over a run.
    auto roles = run(777);
    int intercepts = 0;
    for (const auto& r : roles)
        if (r == "intercept")
            ++intercepts;
    CHECK(intercepts >= 1);
}

TEST_CASE("CampaignEngine: a retried story failure stays pending and keeps the lock (#635)", "[campaign][engine]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    CampaignEngine eng(parsed.campaign, 5, syntheticLoader());

    eng.recordOutcome("u01_storm", /*success=*/false); // default on_fail = retry
    CHECK(eng.completedStory().empty());
    CHECK(eng.dynamicLocked()); // lock stays on
    NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Story);
    CHECK(nm.missionId == "u01_storm"); // re-flown
}

TEST_CASE("CampaignEngine: save/restore round-trips the runtime state (#635)", "[campaign][engine]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    CampaignEngine a(parsed.campaign, 99, syntheticLoader());
    a.recordOutcome("u01_storm", true);
    a.recordOutcome(a.nextMission().missionId, true); // one dynamic sortie flown + attrition

    const std::string blob = a.serialize();

    CampaignEngine b(parsed.campaign, 99, syntheticLoader());
    REQUIRE(b.deserialize(blob));
    CHECK(b.sortiesFlown() == a.sortiesFlown());
    CHECK(b.completedStory() == a.completedStory());
    CHECK(b.theaterUnlocked("ukraine") == a.theaterUnlocked("ukraine"));
    CHECK(b.frontlineFraction("ukraine", 0) == Catch::Approx(a.frontlineFraction("ukraine", 0)));
    // The next mission after restore matches (deterministic continuation).
    CHECK(b.nextMission().kind == a.nextMission().kind);
}

// ---------------------------------------------------------------------------
// Dynamic-sortie template materialization (#635)
// ---------------------------------------------------------------------------

// A template mission YAML with a `template:` header and ${...} placeholders in a valid mission body.
const char* kTemplateYaml = R"yaml(
template:
  role: strike
  fills:
    - target_area: { from: frontline, side: enemy, prefer: contested }
    - ingress:      { from: frontline, side: friendly }
    - opfor:        { from: ground_units, side: enemy }
name: "Strike -- ${target_area.name}"
map: ${theater.id}
layer: world_clear
time: { hour: 12, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [nato, russia]
objects:
  - type: SA10
    id: sam1
    side: russia
    pos: ${target_area.pos}
    heading: 0
  - type: F22
    id: player1
    side: nato
    pos: ${ingress.pos}
    heading: 90
    player: true
triggers:
  - on: destroy(sam1)
    do: mission_success
)yaml";

TEST_CASE("materializeMissionTemplate strips the header and substitutes placeholders (#635)", "[campaign][template]") {
    fl::TemplateFills fills;
    fills["target_area"] = {{"name", "ukraine sector"}, {"pos", "[15000.0, 0.0, 9000.0]"}};
    fills["ingress"] = {{"pos", "[12000.0, 0.0, 8000.0]"}};
    fills["theater"] = {{"id", "ukraine"}};

    std::vector<std::string> warnings;
    const std::string out = fl::materializeMissionTemplate(kTemplateYaml, fills, &warnings);

    CHECK(warnings.empty());
    CHECK(out.find("template:") == std::string::npos);      // header stripped
    CHECK(out.find("fills:") == std::string::npos);         // ...and its children
    CHECK(out.find("${") == std::string::npos);             // every placeholder resolved
    CHECK(out.find("ukraine sector") != std::string::npos); // name substituted
    CHECK(out.find("[15000.0, 0.0, 9000.0]") != std::string::npos);

    // The materialized text is a plain, valid mission file.
    auto parsed = parseMission(out);
    for (const auto& e : parsed.errors)
        UNSCOPED_INFO("parse error: " << e);
    REQUIRE(parsed.ok);
    CHECK(parsed.mission.name == "Strike -- ukraine sector");
    CHECK(parsed.mission.map == "ukraine");
    REQUIRE(parsed.mission.objects.size() == 2);
    CHECK(parsed.mission.objects[0].pos[0] == 15000.0);
    CHECK(parsed.mission.objects[1].playerSlot);
}

TEST_CASE("materializeMissionTemplate leaves an unknown placeholder and warns (#635)", "[campaign][template]") {
    fl::TemplateFills fills; // empty
    std::vector<std::string> warnings;
    const std::string out = fl::materializeMissionTemplate("name: ${missing.field}\n", fills, &warnings);
    CHECK(out.find("${missing.field}") != std::string::npos); // left verbatim
    REQUIRE(warnings.size() == 1);
}

TEST_CASE("CampaignEngine: a generated dynamic sortie materializes into a parseable mission (#635)",
          "[campaign][engine][template]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    CampaignEngine eng(parsed.campaign, 7, syntheticLoader());
    eng.recordOutcome("u01_storm", true); // unlock the theater so a dynamic sortie generates

    NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Dynamic);
    // The engine populated structured fills alongside the scalar target/ingress/opfor.
    REQUIRE(nm.fills.count("target_area") == 1);
    CHECK(nm.fills.at("opfor").at("count") == "59"); // russia armor 55 + sam 4
    CHECK(nm.fills.at("theater").at("id") == "ukraine");

    // Materialize a template with the engine's fills and confirm it parses.
    const std::string out = fl::materializeMissionTemplate(kTemplateYaml, nm.fills);
    CHECK(out.find("${") == std::string::npos);
    auto mission = parseMission(out);
    for (const auto& e : mission.errors)
        UNSCOPED_INFO("parse error: " << e);
    REQUIRE(mission.ok);
    CHECK(mission.mission.map == "ukraine");
}

// ---------------------------------------------------------------------------
// CampaignRunner — the end-to-end runnable campaign loop (#635/#584)
// ---------------------------------------------------------------------------

namespace {
// A content loader that serves the story mission files + the dynamic template by path.
CampaignRunner::ContentLoader campaignContent() {
    return [](const std::string& path) -> std::optional<std::string> {
        if (path == "missions/u01.yaml" || path == "missions/u02.yaml") {
            return std::string("name: Story " + path +
                               "\nmap: world\nlayer: world_clear\ntime: { hour: 12, minute: 0 }\n"
                               "wind: { heading: 0, speed: 0 }\nsides: [nato, russia]\n"
                               "objects:\n  - { type: SA10, id: sam1, side: russia, pos: [0,0,0], heading: 0 }\n"
                               "triggers:\n  - on: destroy(sam1)\n    do: mission_success\n");
        }
        if (path == "templates/intercept.yaml" || path == "templates/sead.yaml") {
            return std::string("template:\n  role: strike\n  fills:\n    - target_area: {}\n"
                               "name: Sortie ${theater.id}\nmap: ${theater.id}\nlayer: world_clear\n"
                               "time: { hour: 12, minute: 0 }\nwind: { heading: 0, speed: 0 }\n"
                               "sides: [nato, russia]\n"
                               "objects:\n  - { type: SA10, id: sam1, side: russia, pos: ${target_area.pos}, "
                               "heading: 0 }\ntriggers:\n  - on: destroy(sam1)\n    do: mission_success\n");
        }
        return std::nullopt;
    };
}
} // namespace

TEST_CASE("CampaignRunner: drives a campaign story -> dynamic -> next, each mission parses (#584/#635)",
          "[campaign][runner]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);
    CampaignRunner runner(parsed.campaign, 3, campaignContent(), syntheticLoader());

    // 1) The campaign opens on the campaign_start story mission, and it is a plain parseable mission.
    std::string id;
    auto m0 = runner.nextMissionYaml(id);
    REQUIRE(m0.has_value());
    CHECK(id == "u01_storm");
    REQUIRE(parseMission(*m0).ok);

    // Fly it successfully -> unlocks the theater, arms u02 after 3 sorties.
    runner.recordOutcome(id, true);
    CHECK(runner.engine().theaterUnlocked("ukraine"));

    // 2) Now the runner materializes dynamic sorties from the template; each is a concrete mission.
    for (int i = 0; i < 3; ++i) {
        auto md = runner.nextMissionYaml(id);
        REQUIRE(md.has_value());
        CHECK(md->find("${") == std::string::npos); // fills substituted
        auto mp = parseMission(*md);
        for (const auto& e : mp.errors)
            UNSCOPED_INFO("parse error: " << e);
        REQUIRE(mp.ok);
        runner.recordOutcome(id, true);
    }
    CHECK(runner.engine().sortiesFlown() == 3);

    // 3) After three sorties, the next mission injected is the u02 story beat.
    auto m2 = runner.nextMissionYaml(id);
    REQUIRE(m2.has_value());
    CHECK(id == "u02_iron");
}

TEST_CASE("CampaignRunner: save/restore continues the campaign across a simulated restart (#584/#635)",
          "[campaign][runner]") {
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);

    CampaignRunner a(parsed.campaign, 9, campaignContent(), syntheticLoader());
    std::string id;
    (void)a.nextMissionYaml(id);
    a.recordOutcome(id, true); // fly the story
    (void)a.nextMissionYaml(id);
    a.recordOutcome(id, true); // one dynamic sortie
    const std::string blob = a.save();

    // A fresh process restores the state and continues from where it left off.
    CampaignRunner b(parsed.campaign, 9, campaignContent(), syntheticLoader());
    REQUIRE(b.restore(blob));
    CHECK(b.engine().sortiesFlown() == a.engine().sortiesFlown());
    CHECK(b.engine().theaterUnlocked("ukraine"));
    std::string idB;
    auto next = b.nextMissionYaml(idB);
    REQUIRE(next.has_value());
    REQUIRE(parseMission(*next).ok); // the continuation mission still resolves + parses
}
