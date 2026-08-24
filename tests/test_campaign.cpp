// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine-campaign tests (#635): the frontline raster, the campaign-schema parser, and the deterministic
// theater-graph state machine (story injection, dynamic selection, frontline advance, save/restore).

#include "campaign/CampaignEngine.h"
#include "campaign/CampaignParser.h"
#include "campaign/CampaignRunner.h"
#include "campaign/Frontline.h"
#include "campaign/MissionTemplate.h"
#include "campaign/TemplateHeader.h"
#include "campaign/TheaterManifest.h"
#include "flight/Geodetic.h"       // geodeticAltitude: the honest check on an airborne template spawn
#include "mission/MissionParser.h" // a generated sortie must round-trip through the real mission parser

#include "campaign_fixture.h"
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

TEST_CASE("CampaignEngine: restore continues the RNG and sortie counter instead of rewinding them (#1224)",
          "[campaign][engine]") {
    // The campaign flow flies ONE sortie per process (fl-server --campaign), restoring from the
    // .flsave each run. Without rng/dynamic_counter in the save, every restart rewound the weighted
    // draw to its seed state: the same role and the same "#1" sortie id, forever.
    auto parsed = parseCampaign(kCampaignYaml);
    REQUIRE(parsed.ok);

    // Continuity reference: one engine flying three dynamic sorties in a single process.
    CampaignEngine cont(parsed.campaign, 777, syntheticLoader());
    cont.recordOutcome("u01_storm", true);
    const NextMission c1 = cont.nextMission();
    cont.recordOutcome(c1.missionId, true);
    const NextMission c2 = cont.nextMission();
    cont.recordOutcome(c2.missionId, true);
    const NextMission c3 = cont.nextMission();

    // The same war flown the intended way: serialize after each sortie, restore into a fresh engine.
    CampaignEngine a(parsed.campaign, 777, syntheticLoader());
    a.recordOutcome("u01_storm", true);
    const NextMission r1 = a.nextMission();
    a.recordOutcome(r1.missionId, true);

    CampaignEngine b(parsed.campaign, 777, syntheticLoader());
    REQUIRE(b.deserialize(a.serialize()));
    const NextMission r2 = b.nextMission();
    b.recordOutcome(r2.missionId, true);

    CampaignEngine c(parsed.campaign, 777, syntheticLoader());
    REQUIRE(c.deserialize(b.serialize()));
    const NextMission r3 = c.nextMission();

    // The restored war IS the continuous war: same templates, same ids, in order.
    CHECK(r1.missionId == c1.missionId);
    CHECK(r2.missionId == c2.missionId);
    CHECK(r3.missionId == c3.missionId);
    CHECK(r2.role == c2.role);
    CHECK(r3.role == c3.role);

    // And sortie ids stay unique across restarts (the "#1 forever" collision).
    CHECK(r1.missionId != r2.missionId);
    CHECK(r2.missionId != r3.missionId);
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

// A template that places its player AIRBORNE, the only correct way a template can: object-level
// lat:/lon: from the new geodetic fills plus alt: MSL. The ${...pos} world triples are SEA-LEVEL
// points, and without an anchor a numeric Y is raw world Y — away from the origin neither can put
// an aircraft at altitude, which is why the degrees are exposed at all.
const char* kAirborneTemplateYaml = R"yaml(
template:
  role: intercept
  fills:
    - target_area: { from: frontline, side: enemy }
    - ingress:      { from: frontline, side: friendly }
name: "Intercept over ${target_area.name}"
map: ${theater.id}
layer: world_clear
time: { hour: 12, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [nato, russia]
objects:
  # `pos` stays required by the schema; lat/lon override its horizontal components and `alt` its
  # vertical, so the zeros are inert placeholders.
  - type: F22
    id: player1
    side: nato
    pos: [0, 0, 0]
    lat: ${ingress.lat}
    lon: ${ingress.lon}
    alt: 6000
    heading: 90
    player: true
  - type: SU34
    id: bandit1
    side: russia
    pos: [0, 0, 0]
    lat: ${target_area.lat}
    lon: ${target_area.lon}
    alt: 7000
    heading: 270
triggers:
  - on: destroy(bandit1)
    do: mission_success
)yaml";

TEST_CASE("CampaignEngine: geodetic fills place an airborne template spawn at real altitude",
          "[campaign][engine][template]") {
    // The fixture's u01 win repaints the raster all-friendly, which leaves no enemy cell for the
    // target fill to resolve against — drop the set_frontline so the split raster survives the
    // unlock and BOTH fills resolve.
    std::string yaml = kCampaignYaml;
    const auto sf = yaml.find("      set_frontline: frontlines/ukraine_after_u01.png\n");
    REQUIRE(sf != std::string::npos);
    yaml.erase(sf, std::string("      set_frontline: frontlines/ukraine_after_u01.png\n").size());
    auto parsed = parseCampaign(yaml);
    REQUIRE(parsed.ok);
    // Give the theater real mid-latitude bounds, as the host does from the manifest (#847). The
    // point of the geodetic fills is exactly this case: far from the origin, where world Y is not
    // altitude and a sea-level world triple is below the terrain.
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    parsed.campaign.theaters[0].bounds =
        GeoBounds{44.0 * kDegToRad, 22.0 * kDegToRad, 52.5 * kDegToRad, 40.0 * kDegToRad};
    CampaignEngine eng(parsed.campaign, 7, syntheticLoader());
    eng.recordOutcome("u01_storm", true);

    NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Dynamic);
    REQUIRE(nm.fills.count("target_area") == 1);
    REQUIRE(nm.fills.count("ingress") == 1);

    // The degree fills exist and land inside the theater box.
    const double tLat = std::stod(nm.fills.at("target_area").at("lat"));
    const double tLon = std::stod(nm.fills.at("target_area").at("lon"));
    const double iLat = std::stod(nm.fills.at("ingress").at("lat"));
    const double iLon = std::stod(nm.fills.at("ingress").at("lon"));
    for (double v : {tLat, iLat}) {
        CHECK(v >= 44.0);
        CHECK(v <= 52.5);
    }
    for (double v : {tLon, iLon}) {
        CHECK(v >= 22.0);
        CHECK(v <= 40.0);
    }

    // Materialized, the airborne spawns sit at their commanded MSL altitude — measured geodetically
    // from the resolved world position, not read back from any Y component.
    const std::string out = fl::materializeMissionTemplate(kAirborneTemplateYaml, nm.fills);
    CHECK(out.find("${") == std::string::npos);
    auto mission = parseMission(out);
    for (const auto& e : mission.errors)
        UNSCOPED_INFO("parse error: " << e);
    REQUIRE(mission.ok);
    REQUIRE(mission.mission.objects.size() == 2);
    const auto& p = mission.mission.objects[0];
    const auto& b = mission.mission.objects[1];
    CHECK(geodeticAltitude(p.pos[0], p.pos[1], p.pos[2]) == Catch::Approx(6000.0).margin(2.0));
    CHECK(geodeticAltitude(b.pos[0], b.pos[1], b.pos[2]) == Catch::Approx(7000.0).margin(2.0));
    // And the mid-latitude point is nowhere near the origin — the case the world triples get wrong.
    CHECK(std::abs(p.pos[1]) > 1'000'000.0);
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

// ── Theater manifest + template header (#847) ───────────────────────────────

TEST_CASE("parseTheaterManifest: valid manifest, bounds to radians") {
    auto r = fl::parseTheaterManifest("[theater]\nid = \"ukraine\"\nname = \"Ukraine\"\n"
                                      "bounds = { min_lat = 44.0, min_lon = 22.0, max_lat = 52.0, max_lon = 40.0 }\n");
    REQUIRE(r.ok);
    CHECK(r.theater.id == "ukraine");
    CHECK(r.theater.terrain == "world"); // default
    auto b = fl::theaterGeoBounds(r.theater);
    CHECK(b.minLat == Catch::Approx(44.0 * 3.14159265358979323846 / 180.0));
}

TEST_CASE("parseTheaterManifest: out-of-range and inverted bounds fail") {
    auto r1 = fl::parseTheaterManifest("[theater]\nid = \"x\"\n"
                                       "bounds = { min_lat = 44, min_lon = 22, max_lat = 200, max_lon = 40 }\n");
    CHECK_FALSE(r1.ok); // max_lat 200 out of range
    auto r2 = fl::parseTheaterManifest("[theater]\nid = \"x\"\n"
                                       "bounds = { min_lat = 52, min_lon = 22, max_lat = 44, max_lon = 40 }\n");
    CHECK_FALSE(r2.ok); // min_lat >= max_lat
    auto r3 = fl::parseTheaterManifest("[theater]\nname = \"no id\"\n");
    CHECK_FALSE(r3.ok); // missing id + bounds
}

TEST_CASE("parseTemplateHeader: role + fills sources") {
    auto r = fl::parseTemplateHeader("template:\n"
                                     "  role: intercept\n"
                                     "  fills:\n"
                                     "    - target_area: {from: frontline}\n"
                                     "    - player_flight: {size: 2}\n"
                                     "name: X\n");
    REQUIRE(r.present);
    CHECK(r.role == "intercept");
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[0].name == "target_area");
    CHECK(r.fills[0].from == "frontline");
    CHECK(r.fills[1].name == "player_flight");
    CHECK(r.fills[1].from.empty()); // a literal-parameter fill

    auto none = fl::parseTemplateHeader("name: plain mission\n");
    CHECK_FALSE(none.present);
}

TEST_CASE("splitTemplateHeader: CRLF-authored templates keep their whole header (#1238)") {
    // A blank line inside a CRLF template block arrives as a lone '\r'. The old header extractor
    // terminated on it (so validation saw a truncated header) while the materialize path continued
    // through it — the two walkers disagreed on what document they were reading.
    const char* crlf = "template:\r\n"
                       "  role: intercept\r\n"
                       "\r\n" // blank line INSIDE the header block
                       "  fills:\r\n"
                       "    - target_area: {from: frontline}\r\n"
                       "name: X\r\n"
                       "body: here\r\n";

    auto r = fl::parseTemplateHeader(crlf);
    REQUIRE(r.present);
    CHECK(r.role == "intercept");
    REQUIRE(r.fills.size() == 1); // 0 before #1238: the blank '\r' line cut the header early
    CHECK(r.fills[0].name == "target_area");

    // The body starts at the first column-0 non-blank line, and validate + materialize agree on it.
    auto split = fl::splitTemplateHeader(crlf);
    CHECK(split.body.find("name: X") != std::string::npos);
    CHECK(split.body.find("role:") == std::string::npos);

    // A CR-only "blank" and a plain LF document behave the same way.
    auto lf = fl::splitTemplateHeader("template:\n  role: strike\n\n  fills: []\nname: Y\n");
    CHECK(lf.header.find("fills") != std::string::npos);
    CHECK(lf.body == "name: Y\n");
}

TEST_CASE("parseCampaign: dangling next.id and unreachable story") {
    auto r = fl::parseCampaign("name: C\nsides: [a, b]\npilot:\n  side: a\n"
                               "story:\n"
                               "  - id: s1\n    file: m.yaml\n    trigger: campaign_start\n"
                               "    on_complete:\n      next:\n        id: ghost\n");
    CHECK_FALSE(r.ok); // dangling next.id is an error
}
