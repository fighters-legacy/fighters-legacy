// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/CampaignParser.h"

#include <yaml-cpp/yaml.h>

#include <set>
#include <string>
#include <vector>

namespace fl {

namespace {

// Read a `trigger:` value that may be a scalar ("campaign_start") or a mapping form the YAML sugar
// allows (`{ after_sorties: 3 }`), normalising both to the canonical "after_sorties:N" string form.
std::string normalizeTrigger(const YAML::Node& n) {
    if (!n)
        return {};
    if (n.IsScalar())
        return n.as<std::string>();
    if (n.IsMap()) {
        if (n["after_sorties"])
            return "after_sorties:" + n["after_sorties"].as<std::string>();
        if (n["frontline_reaches"])
            return "frontline_reaches:" + n["frontline_reaches"].as<std::string>();
    }
    return {};
}

} // namespace

CampaignParseResult parseCampaign(std::string_view yamlContent) {
    CampaignParseResult r;
    CampaignDef& c = r.campaign;

    YAML::Node doc;
    try {
        doc = YAML::Load(std::string(yamlContent));
    } catch (const YAML::Exception& e) {
        r.errors.push_back(std::string("YAML parse error: ") + e.what());
        r.ok = false;
        return r;
    }
    if (!doc.IsMap()) {
        r.errors.push_back("campaign document must be a YAML mapping");
        r.ok = false;
        return r;
    }

    // ── name / version ────────────────────────────────────────────────────────
    if (!doc["name"] || !doc["name"].IsScalar())
        r.errors.push_back("missing required field: name");
    else
        c.name = doc["name"].as<std::string>();
    c.version = doc["version"] ? doc["version"].as<std::string>("") : "";

    // ── sides (exactly two; order significant) ───────────────────────────────
    if (!doc["sides"] || !doc["sides"].IsSequence()) {
        r.errors.push_back("missing required field: sides (a two-element sequence)");
    } else if (doc["sides"].size() != 2) {
        r.errors.push_back("sides must have exactly 2 elements (index 0 = side A, 1 = side B)");
    } else {
        c.sides[0] = doc["sides"][0].as<std::string>("");
        c.sides[1] = doc["sides"][1].as<std::string>("");
        if (c.sides[0].empty() || c.sides[1].empty())
            r.errors.push_back("sides entries must be non-empty faction ids");
    }

    // ── pilot ─────────────────────────────────────────────────────────────────
    if (doc["pilot"] && doc["pilot"].IsMap()) {
        const YAML::Node& p = doc["pilot"];
        c.pilotSide = p["side"] ? p["side"].as<std::string>("") : "";
        c.rankTable = p["rank_table"] ? p["rank_table"].as<std::string>("") : "";
        c.persistentStats = p["persistent_stats"] ? p["persistent_stats"].as<bool>(false) : false;
        if (!c.pilotSide.empty() && c.pilotSide != c.sides[0] && c.pilotSide != c.sides[1])
            r.errors.push_back("pilot.side \"" + c.pilotSide + "\" is not one of sides");
    }

    // ── dynamic.theaters ─────────────────────────────────────────────────────
    if (doc["dynamic"] && doc["dynamic"].IsMap()) {
        const YAML::Node& dyn = doc["dynamic"];
        c.dynamicEnabled = dyn["enabled"] ? dyn["enabled"].as<bool>(true) : true;
        if (dyn["theaters"] && dyn["theaters"].IsSequence()) {
            for (const YAML::Node& tn : dyn["theaters"]) {
                CampaignTheater th;
                th.id = tn["id"] ? tn["id"].as<std::string>("") : "";
                if (th.id.empty()) {
                    r.errors.push_back("dynamic.theaters[] entry missing required field: id");
                    continue;
                }
                th.initialFrontline = tn["initial_frontline"] ? tn["initial_frontline"].as<std::string>("") : "";
                if (tn["frontline_grid"] && tn["frontline_grid"].IsMap()) {
                    th.frontlineCols = tn["frontline_grid"]["cols"].as<int>(0);
                    th.frontlineRows = tn["frontline_grid"]["rows"].as<int>(0);
                }
                if (th.frontlineCols <= 0 || th.frontlineRows <= 0)
                    r.errors.push_back("theater \"" + th.id + "\" needs a positive frontline_grid { cols, rows }");
                if (tn["ground_units"] && tn["ground_units"].IsMap()) {
                    for (const auto& sideKv : tn["ground_units"]) {
                        const std::string side = sideKv.first.as<std::string>();
                        if (!sideKv.second.IsMap())
                            continue;
                        for (const auto& unitKv : sideKv.second)
                            th.groundUnits[side][unitKv.first.as<std::string>()] = unitKv.second.as<int>(0);
                    }
                }
                if (tn["templates"] && tn["templates"].IsSequence()) {
                    for (const YAML::Node& tt : tn["templates"]) {
                        CampaignTemplate ct;
                        ct.role = tt["role"] ? tt["role"].as<std::string>("") : "";
                        ct.file = tt["file"] ? tt["file"].as<std::string>("") : "";
                        ct.weight = tt["weight"] ? tt["weight"].as<int>(1) : 1;
                        ct.requiresTag = tt["requires"] ? tt["requires"].as<std::string>("") : "";
                        if (ct.file.empty())
                            r.errors.push_back("theater \"" + th.id + "\" has a template with no file");
                        if (ct.weight < 1)
                            ct.weight = 1;
                        th.templates.push_back(std::move(ct));
                    }
                }
                c.theaters.push_back(std::move(th));
            }
        }
        if (c.dynamicEnabled && c.theaters.empty())
            r.warnings.push_back("dynamic.enabled is true but no theaters are declared");
    }

    // ── story ─────────────────────────────────────────────────────────────────
    if (doc["story"] && doc["story"].IsSequence()) {
        for (const YAML::Node& sn : doc["story"]) {
            CampaignStoryMission sm;
            sm.id = sn["id"] ? sn["id"].as<std::string>("") : "";
            sm.file = sn["file"] ? sn["file"].as<std::string>("") : "";
            sm.label = sn["label"] ? sn["label"].as<std::string>("") : "";
            sm.trigger = normalizeTrigger(sn["trigger"]);
            sm.locksDynamic = sn["locks_dynamic"] ? sn["locks_dynamic"].as<bool>(false) : false;
            sm.theaterId = sn["theater"] ? sn["theater"].as<std::string>("") : "";
            if (sm.id.empty())
                r.errors.push_back("story[] entry missing required field: id");
            if (sm.file.empty())
                r.errors.push_back("story mission \"" + sm.id + "\" missing required field: file");

            if (sn["on_complete"] && sn["on_complete"].IsMap()) {
                const YAML::Node& oc = sn["on_complete"];
                sm.onComplete.setFrontline = oc["set_frontline"] ? oc["set_frontline"].as<std::string>("") : "";
                sm.onComplete.unlock = oc["unlock"] ? oc["unlock"].as<std::string>("") : "";
                if (oc["next"] && oc["next"].IsMap()) {
                    sm.onComplete.nextId = oc["next"]["id"] ? oc["next"]["id"].as<std::string>("") : "";
                    sm.onComplete.nextTrigger = normalizeTrigger(oc["next"]);
                }
            }
            if (sn["on_fail"] && sn["on_fail"].IsMap()) {
                const YAML::Node& of = sn["on_fail"];
                sm.onFail.retry = of["retry"] ? of["retry"].as<bool>(true) : true;
                sm.onFail.setFrontline = of["set_frontline"] ? of["set_frontline"].as<std::string>("") : "";
                sm.onFail.nextId = of["next"] && of["next"]["id"] ? of["next"]["id"].as<std::string>("") : "";
                sm.onFail.unlockDynamic = of["unlock_dynamic"] ? of["unlock_dynamic"].as<bool>(false) : false;
                if (!sm.onFail.setFrontline.empty() || !sm.onFail.nextId.empty())
                    sm.onFail.retry = false; // a setback/branch implies no retry
            }
            c.story.push_back(std::move(sm));
        }
    }

    if (c.theaters.empty() && c.story.empty())
        r.errors.push_back("a campaign must declare at least one dynamic theater or story mission");

    // ── Referential integrity (#847): the engine cannot resolve these, so a broken graph must not
    // load. Also warn on unreachable beats (almost always an authoring bug). ──────────────────────
    std::set<std::string> storyIds, theaterIds;
    for (const auto& s : c.story) {
        if (!s.id.empty() && !storyIds.insert(s.id).second)
            r.errors.push_back("story[] duplicate id: " + s.id);
    }
    for (const auto& t : c.theaters) {
        if (!t.id.empty() && !theaterIds.insert(t.id).second)
            r.errors.push_back("dynamic.theaters[] duplicate id: " + t.id);
    }
    auto storyExists = [&](const std::string& id) { return storyIds.count(id) != 0; };
    auto theaterExists = [&](const std::string& id) { return theaterIds.count(id) != 0; };
    for (const auto& s : c.story) {
        if (!s.onComplete.nextId.empty() && !storyExists(s.onComplete.nextId))
            r.errors.push_back("story '" + s.id + "': on_complete.next.id references unknown story '" +
                               s.onComplete.nextId + "'");
        if (!s.onFail.nextId.empty() && !storyExists(s.onFail.nextId))
            r.errors.push_back("story '" + s.id + "': on_fail.next.id references unknown story '" + s.onFail.nextId +
                               "'");
        if (!s.onComplete.unlock.empty() && !theaterExists(s.onComplete.unlock))
            r.errors.push_back("story '" + s.id + "': on_complete.unlock references unknown theater '" +
                               s.onComplete.unlock + "'");
        if (!s.theaterId.empty() && !theaterExists(s.theaterId))
            r.errors.push_back("story '" + s.id + "': theater references unknown theater '" + s.theaterId + "'");
        // A set_frontline needs a theater to apply to.
        if ((!s.onComplete.setFrontline.empty() || !s.onFail.setFrontline.empty()) && s.theaterId.empty())
            r.errors.push_back("story '" + s.id + "': set_frontline requires a theater: field");
    }
    // Unreachable-story warning: BFS from the trigger-armed roots (a non-empty top-level trigger is
    // what the engine arms at construction) over the next.id edges.
    if (!c.story.empty()) {
        std::set<std::string> reachable;
        std::vector<std::string> queue;
        for (const auto& s : c.story)
            if (!s.trigger.empty() && !s.id.empty() && reachable.insert(s.id).second)
                queue.push_back(s.id);
        auto findStory = [&](const std::string& id) -> const CampaignStoryMission* {
            for (const auto& s : c.story)
                if (s.id == id)
                    return &s;
            return nullptr;
        };
        while (!queue.empty()) {
            const std::string id = queue.back();
            queue.pop_back();
            const CampaignStoryMission* s = findStory(id);
            if (!s)
                continue;
            for (const std::string& nxt : {s->onComplete.nextId, s->onFail.nextId})
                if (!nxt.empty() && reachable.insert(nxt).second)
                    queue.push_back(nxt);
        }
        for (const auto& s : c.story)
            if (!s.id.empty() && reachable.count(s.id) == 0)
                r.warnings.push_back("story '" + s.id + "' is unreachable from any campaign_start / trigger");
    }

    r.ok = r.errors.empty();
    return r;
}

} // namespace fl
