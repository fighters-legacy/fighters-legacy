// SPDX-License-Identifier: GPL-3.0-or-later
//
// CampaignParser validation and referential integrity (#1145). The happy path lives in
// test_campaign.cpp; this file is the other 44% of the file's branches — every way a campaign can be
// malformed, and every cross-reference the engine cannot resolve at runtime.
//
// The parser is the only thing standing between an authoring mistake and a campaign that loads into
// a broken state, so "rejects it AND says why" is the contract, not just "does not crash".

#include <catch2/catch_test_macros.hpp>

#include "campaign/CampaignParser.h"

#include <algorithm>
#include <string>

using namespace fl;

namespace {

// Does any error/warning mention this fragment? Substring rather than equality: the messages embed
// ids and quoting, and pinning them exactly would make every message reword a test failure.
bool mentions(const std::vector<std::string>& msgs, std::string_view needle) {
    return std::any_of(msgs.begin(), msgs.end(),
                       [needle](const std::string& m) { return m.find(needle) != std::string::npos; });
}

// A minimal campaign that parses clean — the base every negative case perturbs.
constexpr const char* kMinimal = R"(
name: Test Campaign
version: "1.0"
sides: [blue, red]
story:
  - id: opener
    file: missions/opener.yaml
    trigger: campaign_start
)";

} // namespace

// ---------------------------------------------------------------------------
// Document shape
// ---------------------------------------------------------------------------

TEST_CASE("parseCampaign: malformed YAML is an error, not a crash (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign("name: [unclosed\n  bad: : :");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "YAML parse error"));
}

TEST_CASE("parseCampaign: a non-mapping document is rejected (#1145)", "[campaign][parser]") {
    for (const char* doc : {"- just\n- a\n- list\n", "a plain scalar\n"}) {
        const auto r = parseCampaign(doc);
        CHECK_FALSE(r.ok);
        CHECK(mentions(r.errors, "must be a YAML mapping"));
    }
}

TEST_CASE("parseCampaign: an empty document is rejected (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign("");
    CHECK_FALSE(r.ok);
}

// ---------------------------------------------------------------------------
// name / sides / pilot
// ---------------------------------------------------------------------------

TEST_CASE("parseCampaign: name is required and must be scalar (#1145)", "[campaign][parser]") {
    const auto missing = parseCampaign("sides: [blue, red]\nstory:\n  - id: a\n    file: a.yaml\n");
    CHECK_FALSE(missing.ok);
    CHECK(mentions(missing.errors, "missing required field: name"));

    // A mapping where a scalar belongs: the same error, not a type-conversion throw.
    const auto notScalar = parseCampaign("name: { a: b }\nsides: [blue, red]\nstory:\n  - id: a\n    file: a.yaml\n");
    CHECK_FALSE(notScalar.ok);
    CHECK(mentions(notScalar.errors, "missing required field: name"));
}

TEST_CASE("parseCampaign: version is optional and defaults empty (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign("name: N\nsides: [blue, red]\nstory:\n  - id: a\n    file: a.yaml\n");
    CHECK(r.campaign.version.empty());
}

TEST_CASE("parseCampaign: sides must be a two-element sequence of non-empty ids (#1145)", "[campaign][parser]") {
    auto check = [](const char* sides, std::string_view expect) {
        const std::string doc =
            std::string("name: N\n") + sides + "story:\n  - id: a\n    file: a.yaml\n    trigger: campaign_start\n";
        const auto r = parseCampaign(doc);
        CHECK_FALSE(r.ok);
        CHECK(mentions(r.errors, expect));
    };
    check("", "missing required field: sides");                 // absent
    check("sides: blue\n", "missing required field: sides");    // scalar, not a sequence
    check("sides: [blue]\n", "exactly 2 elements");             // one
    check("sides: [blue, red, green]\n", "exactly 2 elements"); // three
    check("sides: [\"\", red]\n", "non-empty faction ids");     // empty A
    check("sides: [blue, \"\"]\n", "non-empty faction ids");    // empty B
}

TEST_CASE("parseCampaign: pilot.side must name one of the two sides (#1145)", "[campaign][parser]") {
    const auto bad = parseCampaign(R"(
name: N
sides: [blue, red]
pilot:
  side: green
  rank_table: ranks/usaf.yaml
  persistent_stats: true
story:
  - id: a
    file: a.yaml
    trigger: campaign_start
)");
    CHECK_FALSE(bad.ok);
    CHECK(mentions(bad.errors, "is not one of sides"));

    const auto good = parseCampaign(R"(
name: N
sides: [blue, red]
pilot:
  side: blue
  rank_table: ranks/usaf.yaml
  persistent_stats: true
story:
  - id: a
    file: a.yaml
    trigger: campaign_start
)");
    CHECK(good.ok);
    CHECK(good.campaign.pilotSide == "blue");
    CHECK(good.campaign.rankTable == "ranks/usaf.yaml");
    CHECK(good.campaign.persistentStats);

    // A pilot block that is not a map is ignored rather than fatal.
    const auto scalarPilot = parseCampaign("name: N\nsides: [blue, red]\npilot: nobody\nstory:\n  - id: a\n"
                                           "    file: a.yaml\n    trigger: campaign_start\n");
    CHECK(scalarPilot.campaign.pilotSide.empty());
}

// ---------------------------------------------------------------------------
// dynamic.theaters
// ---------------------------------------------------------------------------

TEST_CASE("parseCampaign: a theater needs an id and a positive frontline grid (#1145)", "[campaign][parser]") {
    const auto noId = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  theaters:
    - initial_frontline: "0,0"
)");
    CHECK_FALSE(noId.ok);
    CHECK(mentions(noId.errors, "missing required field: id"));

    const auto noGrid = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  theaters:
    - id: north
)");
    CHECK_FALSE(noGrid.ok);
    CHECK(mentions(noGrid.errors, "positive frontline_grid"));

    const auto zeroGrid = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  theaters:
    - id: north
      frontline_grid: { cols: 0, rows: 4 }
)");
    CHECK_FALSE(zeroGrid.ok);
    CHECK(mentions(zeroGrid.errors, "positive frontline_grid"));
}

TEST_CASE("parseCampaign: dynamic enabled with no theaters warns rather than fails (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  enabled: true
story:
  - id: a
    file: a.yaml
    trigger: campaign_start
)");
    CHECK(r.ok); // a warning, not an error: the campaign still runs its story
    CHECK(mentions(r.warnings, "no theaters are declared"));
}

TEST_CASE("parseCampaign: ground units and templates parse, and bad templates are caught (#1145)",
          "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  theaters:
    - id: north
      frontline_grid: { cols: 8, rows: 4 }
      ground_units:
        blue:
          armor: 12
          infantry: 30
        red: not-a-map
      templates:
        - role: cap
          file: templates/cap.yaml
          weight: 3
        - role: strike
          file: templates/strike.yaml
          weight: 0
        - role: broken
          weight: 2
)");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "template with no file"));

    REQUIRE(r.campaign.theaters.size() == 1);
    const auto& th = r.campaign.theaters[0];
    CHECK(th.frontlineCols == 8);
    CHECK(th.frontlineRows == 4);
    CHECK(th.groundUnits.at("blue").at("armor") == 12);
    CHECK(th.groundUnits.count("red") == 0); // a non-map side is skipped, not a parse failure
    REQUIRE(th.templates.size() == 3);
    CHECK(th.templates[0].weight == 3);
    CHECK(th.templates[1].weight == 1); // a non-positive weight is clamped to 1, not honoured
}

TEST_CASE("parseCampaign: duplicate theater ids are rejected (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  theaters:
    - id: north
      frontline_grid: { cols: 8, rows: 4 }
    - id: north
      frontline_grid: { cols: 8, rows: 4 }
)");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "duplicate id: north"));
}

// ---------------------------------------------------------------------------
// story
// ---------------------------------------------------------------------------

TEST_CASE("parseCampaign: a story mission needs an id and a file (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - file: a.yaml
  - id: b
)");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "story[] entry missing required field: id"));
    CHECK(mentions(r.errors, "missing required field: file"));
}

TEST_CASE("parseCampaign: duplicate story ids are rejected (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: dup
    file: a.yaml
    trigger: campaign_start
  - id: dup
    file: b.yaml
)");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "duplicate id: dup"));
}

TEST_CASE("parseCampaign: the trigger sugar normalises to the canonical string form (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: scalar
    file: a.yaml
    trigger: campaign_start
  - id: sorties
    file: b.yaml
    trigger: { after_sorties: 3 }
  - id: frontline
    file: c.yaml
    trigger: { frontline_reaches: "north:4" }
  - id: unknown_map
    file: d.yaml
    trigger: { something_else: 1 }
)");
    REQUIRE(r.campaign.story.size() == 4);
    CHECK(r.campaign.story[0].trigger == "campaign_start");
    CHECK(r.campaign.story[1].trigger == "after_sorties:3");
    CHECK(r.campaign.story[2].trigger == "frontline_reaches:north:4");
    CHECK(r.campaign.story[3].trigger.empty()); // an unrecognised mapping form yields no trigger
}

TEST_CASE("parseCampaign: on_fail retry defaults true and a setback implies no retry (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  theaters:
    - id: north
      frontline_grid: { cols: 8, rows: 4 }
story:
  - id: plain
    file: a.yaml
    trigger: campaign_start
    on_fail: { unlock_dynamic: true }
  - id: setback
    file: b.yaml
    theater: north
    on_fail: { set_frontline: "2,2" }
  - id: branch
    file: c.yaml
    on_fail: { next: { id: plain } }
)");
    REQUIRE(r.campaign.story.size() == 3);
    CHECK(r.campaign.story[0].onFail.retry); // nothing to branch to: retry the sortie
    CHECK(r.campaign.story[0].onFail.unlockDynamic);
    CHECK_FALSE(r.campaign.story[1].onFail.retry); // a frontline setback is the consequence
    CHECK_FALSE(r.campaign.story[2].onFail.retry); // so is a branch to another mission
}

TEST_CASE("parseCampaign: on_complete next/unlock/set_frontline parse (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
dynamic:
  theaters:
    - id: north
      frontline_grid: { cols: 8, rows: 4 }
story:
  - id: first
    file: a.yaml
    theater: north
    trigger: campaign_start
    on_complete:
      set_frontline: "3,3"
      unlock: north
      next: { id: second, after_sorties: 2 }
  - id: second
    file: b.yaml
)");
    CHECK(r.ok);
    REQUIRE(r.campaign.story.size() == 2);
    const auto& oc = r.campaign.story[0].onComplete;
    CHECK(oc.setFrontline == "3,3");
    CHECK(oc.unlock == "north");
    CHECK(oc.nextId == "second");
    CHECK(oc.nextTrigger == "after_sorties:2");
}

// ---------------------------------------------------------------------------
// Referential integrity (#847) — the graph the engine cannot resolve at runtime
// ---------------------------------------------------------------------------

TEST_CASE("parseCampaign: a campaign with neither theaters nor story is rejected (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign("name: N\nsides: [blue, red]\n");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "at least one dynamic theater or story mission"));
}

TEST_CASE("parseCampaign: dangling story references are rejected (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: a
    file: a.yaml
    trigger: campaign_start
    on_complete: { next: { id: ghost } }
  - id: b
    file: b.yaml
    on_fail: { next: { id: phantom } }
)");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "unknown story 'ghost'"));
    CHECK(mentions(r.errors, "unknown story 'phantom'"));
}

TEST_CASE("parseCampaign: dangling theater references are rejected (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: a
    file: a.yaml
    trigger: campaign_start
    theater: nowhere
    on_complete: { unlock: alsonowhere }
)");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "unknown theater 'nowhere'"));
    CHECK(mentions(r.errors, "unknown theater 'alsonowhere'"));
}

TEST_CASE("parseCampaign: set_frontline without a theater is rejected (#1145)", "[campaign][parser]") {
    // There is nothing for the frontline move to apply to — at runtime this would silently do
    // nothing, which is the worst possible outcome for a campaign beat.
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: a
    file: a.yaml
    trigger: campaign_start
    on_complete: { set_frontline: "1,1" }
)");
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, "set_frontline requires a theater"));
}

TEST_CASE("parseCampaign: an unreachable story beat warns (#1145)", "[campaign][parser]") {
    // Reachability is a BFS from the trigger-armed roots over next.id edges. `orphan` has no
    // trigger and nothing points at it, so it can never run — almost always an authoring slip.
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: root
    file: a.yaml
    trigger: campaign_start
    on_complete: { next: { id: middle } }
  - id: middle
    file: b.yaml
  - id: orphan
    file: c.yaml
)");
    CHECK(r.ok); // a warning: an unreachable beat is not a broken graph
    CHECK(mentions(r.warnings, "'orphan' is unreachable"));
    CHECK_FALSE(mentions(r.warnings, "'middle' is unreachable")); // reached through the edge
    CHECK_FALSE(mentions(r.warnings, "'root' is unreachable"));
}

TEST_CASE("parseCampaign: on_fail edges also make a beat reachable (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: root
    file: a.yaml
    trigger: campaign_start
    on_fail: { next: { id: consolation } }
  - id: consolation
    file: b.yaml
)");
    CHECK(r.ok);
    CHECK_FALSE(mentions(r.warnings, "unreachable"));
}

TEST_CASE("parseCampaign: a story with no triggers at all is entirely unreachable (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(R"(
name: N
sides: [blue, red]
story:
  - id: a
    file: a.yaml
  - id: b
    file: b.yaml
)");
    CHECK(mentions(r.warnings, "'a' is unreachable"));
    CHECK(mentions(r.warnings, "'b' is unreachable"));
}

TEST_CASE("parseCampaign: a clean minimal campaign passes with no diagnostics (#1145)", "[campaign][parser]") {
    const auto r = parseCampaign(kMinimal);
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty());
    CHECK(r.campaign.name == "Test Campaign");
    CHECK(r.campaign.version == "1.0");
    CHECK(r.campaign.sides[0] == "blue");
    CHECK(r.campaign.sides[1] == "red");
}
