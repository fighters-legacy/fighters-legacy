// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission_validator.h"

#include <yaml-cpp/yaml.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

// ── bounds constants ──────────────────────────────────────────────────────────

static constexpr int kTimeHourMin = 0;
static constexpr int kTimeHourMax = 23;
static constexpr int kTimeMinuteMin = 0;
static constexpr int kTimeMinuteMax = 59;
static constexpr int kWindHeadingMin = 0;
static constexpr int kWindHeadingMax = 359;
static constexpr int kPosComponents = 3;
static constexpr int kSidesMinCount = 1;
static constexpr int kObjectsMinCount = 1;

// ── helpers ───────────────────────────────────────────────────────────────────

static bool hasKey(const YAML::Node& node, const char* key) {
    return node[key] && !node[key].IsNull();
}

MissionValidationResult validateMission(std::string_view yamlContent) {
    MissionValidationResult r;

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
            int m = time["minute"].as<int>(-1);
            if (m < kTimeMinuteMin || m > kTimeMinuteMax) {
                r.errors.push_back("time.minute must be in [" + std::to_string(kTimeMinuteMin) + ", " +
                                   std::to_string(kTimeMinuteMax) + "] (got " + std::to_string(m) + ")");
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
            if (s < 0.0) {
                r.errors.push_back("wind.speed must be >= 0");
                r.ok = false;
            }
        }
    }

    // ── sides ─────────────────────────────────────────────────────────────────
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
        for (const auto& s : doc["sides"])
            knownSides.insert(s.as<std::string>(""));
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
            // required: type, id, side, pos, heading
            for (const char* field : {"type", "id", "side", "heading"}) {
                if (!hasKey(obj, field)) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "] missing required field: " + field);
                    r.ok = false;
                }
            }
            // id must be unique
            if (hasKey(obj, "id")) {
                std::string id = obj["id"].as<std::string>("");
                if (!knownIds.insert(id).second) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].id \"" + id + "\" is duplicated");
                    r.ok = false;
                }
            }
            // side must be in sides list
            if (hasKey(obj, "side") && !knownSides.empty()) {
                std::string side = obj["side"].as<std::string>("");
                if (knownSides.find(side) == knownSides.end()) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].side \"" + side +
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
            }
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
            if (!hasKey(trig, "on")) {
                r.errors.push_back("triggers[" + std::to_string(idx) + "] missing required field: on");
                r.ok = false;
            } else {
                std::string onStr = trig["on"].as<std::string>("");
                std::smatch m;
                if (std::regex_match(onStr, m, kDestroyRe)) {
                    std::string refId = m[1].str();
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
            }
            ++idx;
        }
    }

    return r;
}
