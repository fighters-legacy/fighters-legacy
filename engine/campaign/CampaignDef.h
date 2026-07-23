// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The parsed campaign definition (#635) — the engine-side representation of a campaign YAML file
// (schema: docs/modding/formats.md "Campaign Files"). A campaign is a theater graph with two mission
// sources: a dynamic generator driven by frontline state, and a story list fired by triggers. Produced
// by parseCampaign() (CampaignParser.h) and consumed by CampaignEngine (the deterministic runtime).

#include "campaign/Frontline.h" // GeoBounds

#include <array>
#include <map>
#include <string>
#include <vector>

namespace fl {

// A dynamic mission template the generator draws from. The template file itself is a normal mission
// YAML with a `template:` header; here we hold only the campaign-side selection metadata.
struct CampaignTemplate {
    std::string role;        // free-form sortie tag (intercept/cap/strike/sead/…)
    std::string file;        // template mission YAML path (pack-relative)
    int weight{1};           // relative selection weight among eligible templates
    std::string requiresTag; // optional precondition tag; empty = always eligible
};

// One theater in the campaign graph.
struct CampaignTheater {
    std::string id;               // must match a theater manifest id
    std::string initialFrontline; // frontline PNG applied at campaign start
    int frontlineCols{0};
    int frontlineRows{0};
    // Geographic bounds (radians), filled by the host from the theater manifest (#847); zero until the
    // manifest resolves. CampaignEngine passes them to Frontline so a raster maps onto real lat/lon.
    GeoBounds bounds{};
    // Starting order of battle per side: side id -> (unit tag -> count). Free-form counters the
    // generator scales force composition from and decrements as missions resolve.
    std::map<std::string, std::map<std::string, int>> groundUnits;
    std::vector<CampaignTemplate> templates;
};

// What a story mission applies when it completes successfully.
struct StoryOnComplete {
    std::string setFrontline; // frontline PNG that replaces the theater's frontline (empty = unchanged)
    std::string unlock;       // theater id to add to the dynamic rotation (empty = none)
    std::string nextId;       // the next story mission id to arm (empty = none)
    std::string nextTrigger;  // the next mission's trigger, e.g. "after_sorties:3" (empty = campaign_start)
};

// What a story mission applies when it fails. Default: retry (stays pending, dynamic stays locked).
struct StoryOnFail {
    bool retry{true};
    std::string setFrontline;  // a setback frontline instead of retrying (implies retry=false)
    std::string nextId;        // branch to a different story mission on failure (implies retry=false)
    bool unlockDynamic{false}; // lift the lock and resume the dynamic war despite the failure
};

struct CampaignStoryMission {
    std::string id;
    std::string file;
    std::string label;
    std::string trigger;      // "campaign_start" | "after_sorties:N" | "frontline_reaches:<tag>" | ""
    bool locksDynamic{false}; // while pending, the dynamic side stops for this mission's theater
    std::string theaterId;    // theater this mission belongs to (for locks_dynamic scoping)
    StoryOnComplete onComplete;
    StoryOnFail onFail;
};

struct CampaignDef {
    std::string name;
    std::string version;
    std::array<std::string, 2> sides{}; // index 0 = side A, index 1 = side B (frontline raster order)
    std::string pilotSide;
    std::string rankTable;
    bool persistentStats{false};

    bool dynamicEnabled{false};
    std::vector<CampaignTheater> theaters;
    std::vector<CampaignStoryMission> story;
};

} // namespace fl
