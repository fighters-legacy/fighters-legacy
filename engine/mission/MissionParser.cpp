// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission/MissionParser.h"

#include <yaml-cpp/yaml.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fl {

// ── bounds constants (shared schema — mirror validate-mission's former inline checks) ──────────

static constexpr int kTimeHourMin = 0;
static constexpr int kTimeHourMax = 23;
static constexpr int kTimeMinuteMin = 0;
static constexpr int kTimeMinuteMax = 59;
static constexpr int kWindHeadingMin = 0;
static constexpr int kWindHeadingMax = 359;
static constexpr int kPosComponents = 3;
static constexpr int kSidesMinCount = 1;
static constexpr int kObjectsMinCount = 1;

// ── helpers ────────────────────────────────────────────────────────────────────────────────────

namespace {

bool hasKey(const YAML::Node& node, const char* key) {
    return node[key] && !node[key].IsNull();
}

// Maps a weather preset string to the engine enum. std::nullopt marks an unknown value (the caller
// records the schema error); the string vocabulary matches the WeatherPreset enum in WeatherTypes.h.
std::optional<WeatherPreset> weatherPresetFromString(const std::string& p) {
    if (p == "clear")
        return WeatherPreset::Clear;
    if (p == "partly_cloudy")
        return WeatherPreset::PartlyCloudy;
    if (p == "overcast")
        return WeatherPreset::Overcast;
    if (p == "rain")
        return WeatherPreset::Rain;
    if (p == "storm")
        return WeatherPreset::Storm;
    if (p == "snow")
        return WeatherPreset::Snow;
    if (p == "blizzard")
        return WeatherPreset::Blizzard;
    return std::nullopt;
}

} // namespace

MissionParseResult parseMission(std::string_view yamlContent) {
    MissionParseResult r;
    Mission& m = r.mission;

    YAML::Node doc;
    try {
        doc = YAML::Load(std::string(yamlContent));
    } catch (const YAML::Exception& e) {
        r.errors.push_back(std::string("YAML parse error: ") + e.what());
        r.ok = false;
        return r;
    }

    if (!doc.IsMap()) {
        r.errors.push_back("mission document must be a YAML mapping");
        r.ok = false;
        return r;
    }

    // ── required string fields ────────────────────────────────────────────────
    for (const char* key : {"name", "map", "layer"}) {
        if (!hasKey(doc, key)) {
            r.errors.push_back(std::string("missing required field: ") + key);
            r.ok = false;
        } else if (!doc[key].IsScalar()) {
            r.errors.push_back(std::string(key) + " must be a string");
            r.ok = false;
        }
    }
    if (hasKey(doc, "name") && doc["name"].IsScalar())
        m.name = doc["name"].as<std::string>("");
    if (hasKey(doc, "map") && doc["map"].IsScalar())
        m.map = doc["map"].as<std::string>("");
    if (hasKey(doc, "layer") && doc["layer"].IsScalar())
        m.layer = doc["layer"].as<std::string>("");

    // ── time ──────────────────────────────────────────────────────────────────
    if (!hasKey(doc, "time")) {
        r.errors.push_back("missing required field: time");
        r.ok = false;
    } else {
        auto time = doc["time"];
        if (!hasKey(time, "hour")) {
            r.errors.push_back("missing time.hour");
            r.ok = false;
        } else {
            int h = time["hour"].as<int>(-1);
            m.time.hour = h;
            if (h < kTimeHourMin || h > kTimeHourMax) {
                r.errors.push_back("time.hour must be in [" + std::to_string(kTimeHourMin) + ", " +
                                   std::to_string(kTimeHourMax) + "] (got " + std::to_string(h) + ")");
                r.ok = false;
            }
        }
        if (!hasKey(time, "minute")) {
            r.errors.push_back("missing time.minute");
            r.ok = false;
        } else {
            int mn = time["minute"].as<int>(-1);
            m.time.minute = mn;
            if (mn < kTimeMinuteMin || mn > kTimeMinuteMax) {
                r.errors.push_back("time.minute must be in [" + std::to_string(kTimeMinuteMin) + ", " +
                                   std::to_string(kTimeMinuteMax) + "] (got " + std::to_string(mn) + ")");
                r.ok = false;
            }
        }
    }

    // ── wind ──────────────────────────────────────────────────────────────────
    if (!hasKey(doc, "wind")) {
        r.errors.push_back("missing required field: wind");
        r.ok = false;
    } else {
        auto wind = doc["wind"];
        if (!hasKey(wind, "heading")) {
            r.errors.push_back("missing wind.heading");
            r.ok = false;
        } else {
            int h = wind["heading"].as<int>(-1);
            m.wind.headingDeg = static_cast<float>(h);
            if (h < kWindHeadingMin || h > kWindHeadingMax) {
                r.errors.push_back("wind.heading must be in [" + std::to_string(kWindHeadingMin) + ", " +
                                   std::to_string(kWindHeadingMax) + "] (got " + std::to_string(h) + ")");
                r.ok = false;
            }
        }
        if (!hasKey(wind, "speed")) {
            r.errors.push_back("missing wind.speed");
            r.ok = false;
        } else {
            double s = wind["speed"].as<double>(-1.0);
            m.wind.speedMs = static_cast<float>(s);
            if (s < 0.0) {
                r.errors.push_back("wind.speed must be >= 0");
                r.ok = false;
            }
        }
    }

    // ── weather (optional) ────────────────────────────────────────────────────
    if (hasKey(doc, "weather")) {
        auto weather = doc["weather"];
        if (hasKey(weather, "preset")) {
            std::string p = weather["preset"].as<std::string>("");
            auto preset = weatherPresetFromString(p);
            if (preset) {
                m.weatherPreset = *preset;
            } else {
                r.errors.push_back(
                    "weather.preset must be clear|partly_cloudy|overcast|rain|storm|snow|blizzard (got \"" + p + "\")");
                r.ok = false;
            }
        }
    }

    // ── time_scale (optional) ─────────────────────────────────────────────────
    if (hasKey(doc, "time_scale")) {
        double ts = doc["time_scale"].as<double>(-1.0);
        m.timeScale = static_cast<float>(ts);
        if (ts <= 0.0) {
            r.errors.push_back("time_scale must be > 0 (got " + std::to_string(ts) + ")");
            r.ok = false;
        }
    }

    // ── sides ─────────────────────────────────────────────────────────────────
    // A side is either a scalar id (`sides: [nato, russia]`) or a mapping with an id and optional
    // coalition `allies` list (`sides: [{id: nato, allies: [ukraine]}, ...]`).
    std::set<std::string> knownSides;
    if (!hasKey(doc, "sides")) {
        r.errors.push_back("missing required field: sides");
        r.ok = false;
    } else if (!doc["sides"].IsSequence()) {
        r.errors.push_back("sides must be a sequence");
        r.ok = false;
    } else if (static_cast<int>(doc["sides"].size()) < kSidesMinCount) {
        r.errors.push_back("sides must have at least " + std::to_string(kSidesMinCount) + " element");
        r.ok = false;
    } else {
        std::size_t sidx = 0;
        for (const auto& s : doc["sides"]) {
            MissionSide side;
            if (s.IsScalar()) {
                side.id = s.as<std::string>("");
            } else if (s.IsMap()) {
                if (!hasKey(s, "id")) {
                    r.errors.push_back("sides[" + std::to_string(sidx) + "] missing required field: id");
                    r.ok = false;
                } else {
                    side.id = s["id"].as<std::string>("");
                }
                if (hasKey(s, "allies")) {
                    if (!s["allies"].IsSequence()) {
                        r.errors.push_back("sides[" + std::to_string(sidx) + "].allies must be a sequence");
                        r.ok = false;
                    } else {
                        for (const auto& a : s["allies"])
                            side.allies.push_back(a.as<std::string>(""));
                    }
                }
            } else {
                r.errors.push_back("sides[" + std::to_string(sidx) + "] must be a string or a mapping");
                r.ok = false;
            }
            if (!side.id.empty())
                knownSides.insert(side.id);
            m.sides.push_back(std::move(side));
            ++sidx;
        }

        // Coalition cross-check: every ally must name a known side; self-ally is a no-op (warn).
        for (std::size_t i = 0; i < m.sides.size(); ++i) {
            for (const auto& ally : m.sides[i].allies) {
                if (ally == m.sides[i].id) {
                    r.warnings.push_back("sides[" + std::to_string(i) + "].allies lists itself (\"" + ally +
                                         "\") — ignored");
                } else if (knownSides.find(ally) == knownSides.end()) {
                    r.errors.push_back("sides[" + std::to_string(i) + "].allies references unknown side \"" + ally +
                                       "\"");
                    r.ok = false;
                }
            }
        }
    }

    // ── objects ───────────────────────────────────────────────────────────────
    std::set<std::string> knownIds;
    if (!hasKey(doc, "objects")) {
        r.errors.push_back("missing required field: objects");
        r.ok = false;
    } else if (!doc["objects"].IsSequence()) {
        r.errors.push_back("objects must be a sequence");
        r.ok = false;
    } else if (static_cast<int>(doc["objects"].size()) < kObjectsMinCount) {
        r.errors.push_back("objects must have at least " + std::to_string(kObjectsMinCount) + " element");
        r.ok = false;
    } else {
        std::size_t idx = 0;
        for (const auto& obj : doc["objects"]) {
            if (!obj.IsMap()) {
                r.errors.push_back("objects[" + std::to_string(idx) + "] is not a mapping");
                r.ok = false;
                ++idx;
                continue;
            }
            MissionObject mo;
            // required: type, id, side, pos, heading
            for (const char* field : {"type", "id", "side", "heading"}) {
                if (!hasKey(obj, field)) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "] missing required field: " + field);
                    r.ok = false;
                }
            }
            if (hasKey(obj, "type"))
                mo.type = obj["type"].as<std::string>("");
            if (hasKey(obj, "heading"))
                mo.headingDeg = obj["heading"].as<float>(0.f);
            // id must be unique
            if (hasKey(obj, "id")) {
                mo.id = obj["id"].as<std::string>("");
                if (!knownIds.insert(mo.id).second) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].id \"" + mo.id + "\" is duplicated");
                    r.ok = false;
                }
            }
            // side must be in sides list
            if (hasKey(obj, "side")) {
                mo.side = obj["side"].as<std::string>("");
                if (!knownSides.empty() && knownSides.find(mo.side) == knownSides.end()) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].side \"" + mo.side +
                                       "\" is not in the sides list");
                    r.ok = false;
                }
            }
            // pos: must be sequence of exactly kPosComponents numbers
            if (!hasKey(obj, "pos")) {
                r.errors.push_back("objects[" + std::to_string(idx) + "] missing required field: pos");
                r.ok = false;
            } else if (!obj["pos"].IsSequence()) {
                r.errors.push_back("objects[" + std::to_string(idx) + "].pos must be a sequence");
                r.ok = false;
            } else if (static_cast<int>(obj["pos"].size()) != kPosComponents) {
                r.errors.push_back("objects[" + std::to_string(idx) + "].pos must have exactly " +
                                   std::to_string(kPosComponents) + " components (got " +
                                   std::to_string(obj["pos"].size()) + ")");
                r.ok = false;
            } else {
                for (int c = 0; c < kPosComponents; ++c)
                    mo.pos[c] = obj["pos"][c].as<double>(0.0);
            }
            // alt (optional) — overrides pos[1] at spawn
            if (hasKey(obj, "alt"))
                mo.alt = obj["alt"].as<float>(0.f);
            // speed (optional) — initial airspeed (m/s) along heading; must be >= 0 (#883)
            if (hasKey(obj, "speed")) {
                float sp = obj["speed"].as<float>(-1.f);
                mo.speed = sp;
                if (sp < 0.f) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].speed must be >= 0 (got " +
                                       std::to_string(sp) + ")");
                    r.ok = false;
                }
            }
            // start (optional) — `ground` (parked on the runway) or `air` (default, dropped in) (#885)
            if (hasKey(obj, "start")) {
                const std::string st = obj["start"].as<std::string>("");
                if (st == "ground") {
                    mo.groundStart = true;
                } else if (st != "air") {
                    r.errors.push_back("objects[" + std::to_string(idx) +
                                       "].start must be \"air\" or \"ground\" (got \"" + st + "\")");
                    r.ok = false;
                }
            }
            // A ground start is parked, so an explicit airborne `speed:` on it is contradictory — warn.
            if (mo.groundStart && mo.speed && *mo.speed > 0.f)
                r.warnings.push_back("objects[" + std::to_string(idx) +
                                     "] has start: ground with a non-zero speed; the speed is ignored (parked)");
            // player (optional) — a joinable slot rather than a spawned world entity
            if (hasKey(obj, "player"))
                mo.playerSlot = obj["player"].as<bool>(false);

            // ── scripted-bot fields (#855): ai / route / loadout ──────────────
            if (hasKey(obj, "ai")) {
                if (!obj["ai"].IsScalar()) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].ai must be a string");
                    r.ok = false;
                } else {
                    mo.ai = obj["ai"].as<std::string>("");
                    if (mo.ai.empty())
                        r.warnings.push_back("objects[" + std::to_string(idx) + "].ai is empty — ignored");
                }
            }
            if (hasKey(obj, "route")) {
                if (!obj["route"].IsSequence()) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].route must be a sequence of waypoints");
                    r.ok = false;
                } else {
                    std::size_t w = 0;
                    for (const auto& wp : obj["route"]) {
                        if (!wp.IsSequence() || static_cast<int>(wp.size()) != kPosComponents) {
                            r.errors.push_back("objects[" + std::to_string(idx) + "].route[" + std::to_string(w) +
                                               "] must have exactly " + std::to_string(kPosComponents) + " components");
                            r.ok = false;
                        } else {
                            std::array<double, 3> pt{};
                            for (int c = 0; c < kPosComponents; ++c)
                                pt[static_cast<std::size_t>(c)] = wp[c].as<double>(0.0);
                            mo.route.push_back(pt);
                        }
                        ++w;
                    }
                }
            }
            if (hasKey(obj, "loadout")) {
                if (!obj["loadout"].IsSequence()) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].loadout must be a sequence of weapon ids");
                    r.ok = false;
                } else {
                    for (const auto& store : obj["loadout"])
                        mo.loadout.push_back(store.as<std::string>(""));
                }
            }
            // A player slot is flown by a human, so AI/route/loadout on it are contradictory — warn but
            // do not reject (the slot simply ignores them).
            if (mo.playerSlot && (!mo.ai.empty() || !mo.route.empty() || !mo.loadout.empty()))
                r.warnings.push_back("objects[" + std::to_string(idx) +
                                     "] is a player slot; its ai/route/loadout are ignored");
            if (!mo.ai.empty() && !mo.route.empty())
                r.warnings.push_back("objects[" + std::to_string(idx) +
                                     "] has both ai and route; route takes "
                                     "precedence");

            m.objects.push_back(std::move(mo));
            ++idx;
        }
    }

    // ── triggers ──────────────────────────────────────────────────────────────
    if (!hasKey(doc, "triggers")) {
        r.errors.push_back("missing required field: triggers");
        r.ok = false;
    } else if (!doc["triggers"].IsSequence()) {
        r.errors.push_back("triggers must be a sequence");
        r.ok = false;
    } else {
        static const std::regex kDestroyRe(R"(^destroy\(([^)]+)\)$)");
        std::size_t idx = 0;
        for (const auto& trig : doc["triggers"]) {
            if (!trig.IsMap()) {
                r.errors.push_back("triggers[" + std::to_string(idx) + "] is not a mapping");
                r.ok = false;
                ++idx;
                continue;
            }
            MissionTrigger mt;
            if (!hasKey(trig, "on")) {
                r.errors.push_back("triggers[" + std::to_string(idx) + "] missing required field: on");
                r.ok = false;
            } else {
                mt.on = trig["on"].as<std::string>("");
                std::smatch mtch;
                if (std::regex_match(mt.on, mtch, kDestroyRe)) {
                    std::string refId = mtch[1].str();
                    if (knownIds.find(refId) == knownIds.end()) {
                        r.errors.push_back("triggers[" + std::to_string(idx) + "].on references unknown object id \"" +
                                           refId + "\"");
                        r.ok = false;
                    }
                }
            }
            if (!hasKey(trig, "do")) {
                r.errors.push_back("triggers[" + std::to_string(idx) + "] missing required field: do");
                r.ok = false;
            } else {
                mt.doAction = trig["do"].as<std::string>("");
            }
            m.triggers.push_back(std::move(mt));
            ++idx;
        }
    }

    return r;
}

} // namespace fl
