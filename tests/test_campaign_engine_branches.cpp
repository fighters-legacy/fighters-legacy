// SPDX-License-Identifier: GPL-3.0-or-later
//
// CampaignEngine: the failure policies, the eligibility rules, and a save file that has drifted
// (#1145).
//
// test_campaign.cpp covers a campaign that goes well — story, unlock, dynamic sorties, save/restore.
// What it does not cover is any of the three ways a story mission can FAIL other than "retry it", the
// rules that make a template ineligible, or a save blob written by a different build.
//
// Save/restore is the part with the sharpest edge. A campaign save is the player's progress, and this
// blob is plain text with no version field, so the only thing standing between a renamed story id and
// a lost campaign is that deserialize skips what it does not recognise instead of bailing out.

#include "campaign_fixture.h"
#include <catch2/catch_approx.hpp>

#include <catch2/catch_test_macros.hpp>

#include "campaign/CampaignEngine.h"
#include "campaign/CampaignParser.h"
#include "campaign/Frontline.h"

#include <string>
#include <vector>

using Catch::Approx;
using namespace fl;

namespace {

CampaignDef parse(const char* yaml) {
    auto r = parseCampaign(yaml);
    for (const auto& e : r.errors)
        UNSCOPED_INFO("parse error: " << e);
    REQUIRE(r.ok);
    return r.campaign;
}

// A campaign whose one story mission can be given any on_fail policy.
std::string campaignWithFail(const std::string& onFail) {
    return R"yaml(
name: "Fail Policies"
sides: [nato, russia]
pilot: { side: nato }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units:
        russia: { armor: 3 }
      templates:
        - { role: intercept, file: templates/intercept.yaml, weight: 1 }
story:
  - id: u01
    file: missions/u01.yaml
    trigger: campaign_start
    locks_dynamic: true
    theater: ukraine
)yaml" + onFail +
           R"yaml(
  - id: u02_recover
    file: missions/u02.yaml
)yaml";
}

} // namespace

// ---------------------------------------------------------------------------
// on_fail policies
// ---------------------------------------------------------------------------

TEST_CASE("CampaignEngine: a failure with a set_frontline is a setback, not a retry (#1145)", "[campaign][engine]") {
    // Losing the mission moves the line the wrong way and the story is DONE. Re-flying it would let
    // the player grind the setback away, which is the whole point of authoring a losing branch.
    const std::string yaml = campaignWithFail("    on_fail: { set_frontline: frontlines/after_loss.png }");
    CampaignEngine eng(parse(yaml.c_str()), 1, syntheticLoader());

    REQUIRE(eng.dynamicLocked());
    eng.recordOutcome("u01", /*success=*/false);

    CHECK_FALSE(eng.dynamicLocked());                           // resolved: the war resumes
    CHECK(eng.frontlineFraction("ukraine", 0) == Approx(1.0f)); // the "after" raster was applied
    CHECK(eng.nextMission().kind != NextMission::Kind::Story);  // not re-offered
}

TEST_CASE("CampaignEngine: a failure with a next branches to the recovery mission (#1145)", "[campaign][engine]") {
    const std::string yaml = campaignWithFail("    on_fail: { next: { id: u02_recover } }");
    CampaignEngine eng(parse(yaml.c_str()), 1, syntheticLoader());

    eng.recordOutcome("u01", false);
    const NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Story);
    CHECK(nm.missionId == "u02_recover"); // the branch, armed with no trigger, is immediately eligible
}

TEST_CASE("CampaignEngine: a failure with unlock_dynamic lifts the lock and moves on (#1145)", "[campaign][engine]") {
    // The story is lost, the war continues without it. Nothing else changes.
    const std::string yaml = campaignWithFail("    on_fail: { unlock_dynamic: true }");
    CampaignEngine eng(parse(yaml.c_str()), 7, syntheticLoader());

    REQUIRE(eng.dynamicLocked());
    eng.recordOutcome("u01", false);
    CHECK_FALSE(eng.dynamicLocked());
    CHECK(eng.frontlineFraction("ukraine", 0) == Approx(0.5f)); // untouched
}

TEST_CASE("CampaignEngine: recording an outcome for a mission nobody knows is a no-op (#1145)", "[campaign][engine]") {
    // A save from another campaign, or a mission id the host mangled. It must not throw and must not
    // silently count as a sortie against a theater that does not exist.
    const std::string yaml = campaignWithFail("");
    CampaignEngine eng(parse(yaml.c_str()), 1, syntheticLoader());

    eng.recordOutcome("dynamic:atlantis:strike#1", true); // an unknown theater
    CHECK(eng.sortiesFlown() == 1);                       // still a sortie flown; just no attrition
    eng.recordOutcome("not-a-mission-id", true);
    CHECK(eng.sortiesFlown() == 2);
    CHECK(eng.completedStory().empty());
}

// ---------------------------------------------------------------------------
// Triggers
// ---------------------------------------------------------------------------

TEST_CASE("CampaignEngine: a frontline_reaches story waits for its tag (#1145)", "[campaign][engine]") {
    // The engine deliberately does NOT infer tags from raster contents — content emits them. So the
    // story stays invisible until the host says the milestone happened.
    const char* yaml = R"yaml(
name: "Tags"
sides: [nato, russia]
pilot: { side: nato }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units: { russia: { armor: 3 } }
      templates:
        - { role: intercept, file: templates/intercept.yaml, weight: 1 }
story:
  - id: u01
    file: missions/u01.yaml
    trigger: campaign_start
    theater: ukraine
    on_complete: { next: { frontline_reaches: dnipro, id: u02_river } }
  - id: u02_river
    file: missions/u02.yaml
)yaml";
    CampaignEngine eng(parse(yaml), 3, syntheticLoader());

    eng.recordOutcome("u01", true);
    CHECK(eng.nextMission().kind == NextMission::Kind::Dynamic); // u02 armed but not triggered

    eng.reachFrontlineTag("kharkiv"); // the wrong milestone
    CHECK(eng.nextMission().kind == NextMission::Kind::Dynamic);

    eng.reachFrontlineTag("dnipro");
    const NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Story);
    CHECK(nm.missionId == "u02_river");
}

TEST_CASE("CampaignEngine: a completed story is never re-armed (#1145)", "[campaign][engine]") {
    // A branch pointing back at something already flown, or a tag reached twice. Re-arming would
    // re-offer a mission the player finished.
    const char* yaml = R"yaml(
name: "Rearm"
sides: [nato, russia]
pilot: { side: nato }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units: { russia: { armor: 3 } }
      templates:
        - { role: intercept, file: templates/intercept.yaml, weight: 1 }
story:
  - id: u01
    file: missions/u01.yaml
    trigger: campaign_start
    theater: ukraine
    on_complete: { next: { id: u01 } }
)yaml";
    CampaignEngine eng(parse(yaml), 3, syntheticLoader());
    eng.recordOutcome("u01", true);
    CHECK(eng.completedStory().size() == 1u);
    CHECK(eng.nextMission().kind != NextMission::Kind::Story); // not offered a second time
}

// ---------------------------------------------------------------------------
// Dynamic eligibility
// ---------------------------------------------------------------------------

TEST_CASE("CampaignEngine: a requires tag gates a template on the enemy order of battle (#1145)",
          "[campaign][engine]") {
    // A SEAD sortie against an enemy with no SAMs is a mission with no target. The tag is checked
    // against the LIVE order of battle, so it stops being eligible as attrition removes the units.
    const char* yaml = R"yaml(
name: "Requires"
sides: [nato, russia]
pilot: { side: nato }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units: { russia: { sam: 1 } }
      templates:
        - { role: sead, file: templates/sead.yaml, weight: 1, requires: enemy_sam }
story: []
)yaml";
    CampaignEngine eng(parse(yaml), 11, syntheticLoader());

    const NextMission first = eng.nextMission();
    REQUIRE(first.kind == NextMission::Kind::Dynamic);
    CHECK(first.role == "sead");

    // Fly it successfully: attrition takes the last SAM, and SEAD stops being offered.
    eng.recordOutcome(first.missionId, true);
    CHECK(eng.nextMission().kind == NextMission::Kind::None); // the only template is now ineligible
}

TEST_CASE("CampaignEngine: a theater with no eligible template yields no sortie (#1145)", "[campaign][engine]") {
    const char* yaml = R"yaml(
name: "Nothing eligible"
sides: [nato, russia]
pilot: { side: nato }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units: { russia: { armor: 2 } }
      templates:
        - { role: sead, file: templates/sead.yaml, weight: 1, requires: enemy_carrier }
story: []
)yaml";
    CampaignEngine eng(parse(yaml), 4, syntheticLoader());
    CHECK(eng.nextMission().kind == NextMission::Kind::None);
}

TEST_CASE("CampaignEngine: dynamic disabled means story missions or nothing (#1145)", "[campaign][engine]") {
    const char* yaml = R"yaml(
name: "Story only"
sides: [nato, russia]
pilot: { side: nato }
story:
  - id: u01
    file: missions/u01.yaml
    trigger: campaign_start
)yaml";
    CampaignEngine eng(parse(yaml), 1, syntheticLoader());
    REQUIRE(eng.nextMission().missionId == "u01");
    eng.recordOutcome("u01", true);
    CHECK(eng.nextMission().kind == NextMission::Kind::None); // the campaign is over
}

TEST_CASE("CampaignEngine: the pilot flying the second side inverts friend and foe (#1145)", "[campaign][engine]") {
    // sides[0] is not "the player" — it is just the first side listed. Getting this backwards would
    // send the player to attack their own ground units and count attrition against the wrong army.
    const char* yaml = R"yaml(
name: "Red side"
sides: [nato, russia]
pilot: { side: russia }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units:
        nato:   { armor: 5 }
        russia: { armor: 9 }
      templates:
        - { role: strike, file: templates/strike.yaml, weight: 1 }
story: []
)yaml";
    CampaignEngine eng(parse(yaml), 2, syntheticLoader());

    const NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Dynamic);
    CHECK(nm.opforCount == 5); // NATO's five, not Russia's nine
}

TEST_CASE("CampaignEngine: a locked theater is skipped when generating a sortie (#1145)", "[campaign][engine]") {
    // Two theaters, the second locked behind a story. Only the open one produces sorties.
    const char* yaml = R"yaml(
name: "Two theaters"
sides: [nato, russia]
pilot: { side: nato }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units: { russia: { armor: 9 } }
      templates:
        - { role: intercept, file: templates/intercept.yaml, weight: 1 }
    - id: baltic
      initial_frontline: frontlines/baltic.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units: { russia: { armor: 9 } }
      templates:
        - { role: patrol, file: templates/patrol.yaml, weight: 1 }
story:
  - id: open_baltic
    file: missions/baltic.yaml
    theater: baltic
    on_complete: { unlock: baltic }
)yaml";
    CampaignEngine eng(parse(yaml), 6, syntheticLoader());

    CHECK(eng.theaterUnlocked("ukraine"));
    CHECK_FALSE(eng.theaterUnlocked("baltic"));   // unlocked only by the story
    CHECK_FALSE(eng.theaterUnlocked("atlantis")); // a theater that does not exist is not unlocked

    for (int i = 0; i < 4; ++i) {
        const NextMission nm = eng.nextMission();
        REQUIRE(nm.kind == NextMission::Kind::Dynamic);
        CHECK(nm.theaterId == "ukraine"); // never baltic
        eng.recordOutcome(nm.missionId, true);
    }
}

TEST_CASE("CampaignEngine: querying a theater it does not have is empty, not a crash (#1145)", "[campaign][engine]") {
    const std::string yaml = campaignWithFail("");
    CampaignEngine eng(parse(yaml.c_str()), 1, syntheticLoader());
    CHECK(eng.frontline("atlantis") == nullptr);
    CHECK(eng.frontlineFraction("atlantis", 0) == Approx(0.0f));
    CHECK(eng.frontline("ukraine") != nullptr);
}

TEST_CASE("CampaignEngine: with no frontline loader selection still works, fills are skipped (#1145)",
          "[campaign][engine]") {
    // A headless tool driving the campaign has no image library. It must still be able to ask what
    // to fly next.
    const std::string yaml = campaignWithFail("");
    CampaignEngine eng(parse(yaml.c_str()), 1, /*loader=*/{});

    eng.recordOutcome("u01", true);
    const NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Dynamic);
    CHECK_FALSE(nm.hasFill); // no raster, so no target or ingress point was picked
    CHECK(eng.frontlineFraction("ukraine", 0) == Approx(0.0f));
}

// ---------------------------------------------------------------------------
// The save blob
// ---------------------------------------------------------------------------

TEST_CASE("CampaignEngine: a save blob survives a def whose ids have moved on (#1145)", "[campaign][engine]") {
    // The blob has no version field, so forward and backward compatibility rest entirely on
    // deserialize skipping records it does not recognise. A campaign save IS the player's progress;
    // rejecting the whole file because one story was renamed would delete it.
    const std::string yaml = campaignWithFail("");
    const CampaignDef def = parse(yaml.c_str());
    CampaignEngine eng(def, 1, syntheticLoader());

    const std::string blob = "sorties=12\n"
                             "completed=u01\n"
                             "reached=dnipro,kharkiv\n"
                             "story=u01;0;1;0;campaign_start\n"
                             "story=a_story_from_another_build;1;0;5;campaign_start\n"
                             "theater=ukraine;frontlines/after_win.png;1;russia/armor=2\n"
                             "theater=atlantis;frontlines/nope.png;1;\n"
                             "some_future_key=whatever\n";
    REQUIRE(eng.deserialize(blob));

    CHECK(eng.sortiesFlown() == 12);
    CHECK(eng.completedStory() == std::vector<std::string>{"u01"});
    CHECK(eng.theaterUnlocked("ukraine"));
    CHECK(eng.frontlineFraction("ukraine", 0) == Approx(1.0f)); // the named raster was reloaded
}

TEST_CASE("CampaignEngine: a blob line with no key is rejected (#1145)", "[campaign][engine]") {
    // Skipping unknown KEYS is compatibility; accepting a line that is not a key-value pair at all
    // would be accepting a corrupt file and continuing with half a campaign.
    const std::string yaml = campaignWithFail("");
    CampaignEngine eng(parse(yaml.c_str()), 1, syntheticLoader());

    CHECK_FALSE(eng.deserialize("sorties=3\nthis line has no equals sign\n"));
    CHECK_FALSE(eng.deserialize("theater=ukraine_with_no_semicolon\n"));

    CHECK(eng.deserialize(""));                // an empty blob is a fresh campaign, not corruption
    CHECK(eng.deserialize("\n\nsorties=1\n")); // blank lines are skipped
    CHECK(eng.sortiesFlown() == 1);
}

TEST_CASE("CampaignEngine: malformed ground-unit entries are dropped, not guessed (#1145)", "[campaign][engine]") {
    const std::string yaml = campaignWithFail("");
    CampaignEngine eng(parse(yaml.c_str()), 1, syntheticLoader());

    // "russia/armor=4" is well formed; the rest are not and must not become units.
    REQUIRE(eng.deserialize("theater=ukraine;frontlines/start.png;1;russia/armor=4,nosep,noequals/x,=5\n"));

    // The order of battle drives the opfor count of the next sortie, which is how we can see it.
    eng.recordOutcome("u01", true);
    const NextMission nm = eng.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Dynamic);
    CHECK(nm.opforCount == 4);
}

TEST_CASE("CampaignEngine: serialize round-trips reached tags and armed triggers (#1145)", "[campaign][engine]") {
    // The existing round-trip test covers sorties and completion. Tags and per-story arm state are
    // what decide whether the NEXT story appears, so losing them strands the campaign.
    const char* yaml = R"yaml(
name: "Round trip"
sides: [nato, russia]
pilot: { side: nato }
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/start.png
      frontline_grid: { cols: 8, rows: 4 }
      ground_units: { russia: { armor: 3 } }
      templates:
        - { role: intercept, file: templates/intercept.yaml, weight: 1 }
story:
  - id: u01
    file: missions/u01.yaml
    trigger: campaign_start
    theater: ukraine
    on_complete: { next: { frontline_reaches: dnipro, id: u02 } }
  - id: u02
    file: missions/u02.yaml
)yaml";
    const CampaignDef def = parse(yaml);

    CampaignEngine a(def, 21, syntheticLoader());
    a.recordOutcome("u01", true);
    a.reachFrontlineTag("dnipro");
    REQUIRE(a.nextMission().missionId == "u02");

    CampaignEngine b(def, 21, syntheticLoader());
    REQUIRE(b.deserialize(a.serialize()));
    const NextMission nm = b.nextMission();
    REQUIRE(nm.kind == NextMission::Kind::Story);
    CHECK(nm.missionId == "u02"); // the tag and the arm state both survived
}
