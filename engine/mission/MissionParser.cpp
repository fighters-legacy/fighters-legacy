// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission/MissionParser.h"

#include "world/ZoneGeometry.h" // isConvexPolygonXZ — airspace_zones convexity check (#162)

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
static constexpr float kShotFovMin = 20.f;
static constexpr float kShotFovMax = 120.f;
static constexpr int kMoveKeyframesMinCount = 2;
static constexpr int kZoneVertexComponents = 2; // airspace_zones polygon vertices are [x, z] (#162)
static constexpr int kZonePolygonMinVertices = 3;

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
                // Starting readiness posture (#162); absent = peacetime.
                if (hasKey(s, "alert")) {
                    const std::string lvl = s["alert"].as<std::string>("");
                    if (!alertLevelFromString(lvl, side.alert)) {
                        r.errors.push_back("sides[" + std::to_string(sidx) +
                                           "].alert must be peacetime|elevated|conflict|war_state (got \"" + lvl +
                                           "\")");
                        r.ok = false;
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

    // ── airspace_zones (optional, #162) ───────────────────────────────────────
    // Restricted-airspace regions the AlertSystem enforces. Placed after `sides:` so each zone's
    // `owner:` cross-checks against the side set the same way `objects[].side` does.
    if (hasKey(doc, "airspace_zones")) {
        if (!doc["airspace_zones"].IsSequence()) {
            r.errors.push_back("airspace_zones must be a sequence");
            r.ok = false;
        } else {
            std::set<std::string> knownZoneIds;
            std::size_t zidx = 0;
            for (const auto& zn : doc["airspace_zones"]) {
                const std::string zp = "airspace_zones[" + std::to_string(zidx) + "]";
                ++zidx;
                if (!zn.IsMap()) {
                    r.errors.push_back(zp + " must be a mapping");
                    r.ok = false;
                    continue;
                }

                AirspaceZone zone;
                if (!hasKey(zn, "id")) {
                    r.errors.push_back(zp + " missing required field: id");
                    r.ok = false;
                } else {
                    zone.id = zn["id"].as<std::string>("");
                    if (!knownZoneIds.insert(zone.id).second) {
                        r.errors.push_back(zp + ".id \"" + zone.id + "\" is duplicated");
                        r.ok = false;
                    }
                }

                // Shape dispatch drives which placement fields are required, the `cameras.type`
                // precedent.
                bool shapeValid = false;
                if (!hasKey(zn, "type")) {
                    r.errors.push_back(zp + " missing required field: type");
                    r.ok = false;
                } else {
                    const std::string t = zn["type"].as<std::string>("");
                    if (t == "circle") {
                        zone.shape = ZoneShape::Circle;
                        shapeValid = true;
                    } else if (t == "polygon") {
                        zone.shape = ZoneShape::Polygon;
                        shapeValid = true;
                    } else {
                        r.errors.push_back(zp + ".type must be circle|polygon (got \"" + t + "\")");
                        r.ok = false;
                    }
                }

                if (shapeValid && zone.shape == ZoneShape::Circle) {
                    // `center` is authored as [x, y, z] for symmetry with `objects[].pos`, but the
                    // zone test is XZ + an altitude band, so the Y component is deliberately unused
                    // — alt_floor / alt_ceiling own the vertical extent.
                    if (!hasKey(zn, "center")) {
                        r.errors.push_back(zp + " missing required field: center");
                        r.ok = false;
                    } else if (!zn["center"].IsSequence() || static_cast<int>(zn["center"].size()) != kPosComponents) {
                        r.errors.push_back(zp + ".center must have exactly " + std::to_string(kPosComponents) +
                                           " components [x, y, z]");
                        r.ok = false;
                    } else {
                        zone.centerX = zn["center"][0].as<double>(0.0);
                        zone.centerZ = zn["center"][2].as<double>(0.0);
                    }

                    if (!hasKey(zn, "radius")) {
                        r.errors.push_back(zp + " missing required field: radius");
                        r.ok = false;
                    } else {
                        zone.radiusM = zn["radius"].as<double>(-1.0);
                        if (zone.radiusM <= 0.0) {
                            r.errors.push_back(zp + ".radius must be > 0 metres (got " + std::to_string(zone.radiusM) +
                                               ")");
                            r.ok = false;
                        }
                    }
                } else if (shapeValid) {
                    if (!hasKey(zn, "vertices")) {
                        r.errors.push_back(zp + " missing required field: vertices");
                        r.ok = false;
                    } else if (!zn["vertices"].IsSequence()) {
                        r.errors.push_back(zp + ".vertices must be a sequence of [x, z] pairs");
                        r.ok = false;
                    } else {
                        std::size_t vi = 0;
                        for (const auto& v : zn["vertices"]) {
                            if (!v.IsSequence() || static_cast<int>(v.size()) != kZoneVertexComponents) {
                                r.errors.push_back(zp + ".vertices[" + std::to_string(vi) + "] must have exactly " +
                                                   std::to_string(kZoneVertexComponents) + " components [x, z]");
                                r.ok = false;
                            } else {
                                zone.vertices.emplace_back(v[0].as<double>(0.0), v[1].as<double>(0.0));
                            }
                            ++vi;
                        }
                        if (static_cast<int>(zone.vertices.size()) < kZonePolygonMinVertices) {
                            r.errors.push_back(zp + ".vertices needs at least " +
                                               std::to_string(kZonePolygonMinVertices) + " points");
                            r.ok = false;
                        } else if (!isConvexPolygonXZ(zone.vertices)) {
                            // AirspaceZone documents polygons as convex. A concave or
                            // self-intersecting ring still gets a defined answer at runtime
                            // (crossing-number), but it is far more often a slip than an intent.
                            r.errors.push_back(zp + ".vertices must form a convex polygon");
                            r.ok = false;
                        }
                    }
                }

                if (hasKey(zn, "alt_floor"))
                    zone.altFloorM = zn["alt_floor"].as<double>(zone.altFloorM);
                if (hasKey(zn, "alt_ceiling"))
                    zone.altCeilingM = zn["alt_ceiling"].as<double>(zone.altCeilingM);
                if (zone.altCeilingM <= zone.altFloorM) {
                    r.errors.push_back(zp + ".alt_ceiling must be greater than alt_floor");
                    r.ok = false;
                }

                if (!hasKey(zn, "owner")) {
                    r.errors.push_back(zp + " missing required field: owner");
                    r.ok = false;
                } else {
                    zone.ownerFactionId = zn["owner"].as<std::string>("");
                    if (!knownSides.empty() && knownSides.find(zone.ownerFactionId) == knownSides.end()) {
                        r.errors.push_back(zp + ".owner \"" + zone.ownerFactionId + "\" is not in the sides list");
                        r.ok = false;
                    }
                }

                // `policy` names a content-pack escalation policy. It is optional and NOT
                // cross-checked here: the pack is not loaded at parse time, and a zone with no
                // resolvable policy falls back to the builtin default rather than going inert.
                if (hasKey(zn, "policy"))
                    zone.policyId = zn["policy"].as<std::string>("");

                m.airspaceZones.push_back(std::move(zone));
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
            // ── crew configuration (#976): skill ranges + per-seat overrides ──
            if (hasKey(obj, "crew")) {
                const YAML::Node& crew = obj["crew"];
                if (!crew.IsMap()) {
                    r.errors.push_back("objects[" + std::to_string(idx) + "].crew must be a mapping");
                    r.ok = false;
                } else {
                    MissionCrew mc;
                    const std::string prefix = "objects[" + std::to_string(idx) + "].crew";
                    // A skill value is a single float (fixed) or a [min, max] sequence (range). Fills the
                    // referenced min/max optionals; reports a range error under `where`.
                    auto parseSkill = [&](const YAML::Node& node, const std::string& where,
                                          std::optional<float>& outMin, std::optional<float>& outMax) {
                        if (node.IsSequence()) {
                            if (node.size() != 2u) {
                                r.errors.push_back(where + " range must be [min, max]");
                                r.ok = false;
                                return;
                            }
                            const float lo = node[0].as<float>(-1.f), hi = node[1].as<float>(-1.f);
                            if (lo < 0.f || lo > 1.f || hi < 0.f || hi > 1.f || hi < lo) {
                                r.errors.push_back(where + " must be a [min, max] within [0, 1] with max >= min");
                                r.ok = false;
                                return;
                            }
                            outMin = lo;
                            outMax = hi;
                        } else if (node.IsScalar()) {
                            const float v = node.as<float>(-1.f);
                            if (v < 0.f || v > 1.f) {
                                r.errors.push_back(where + " must be within [0, 1]");
                                r.ok = false;
                                return;
                            }
                            outMin = v;
                            outMax = v;
                        } else {
                            r.errors.push_back(where + " must be a number or a [min, max] range");
                            r.ok = false;
                        }
                    };
                    if (hasKey(crew, "skill"))
                        parseSkill(crew["skill"], prefix + ".skill", mc.skillMin, mc.skillMax);
                    if (hasKey(crew, "seats")) {
                        if (!crew["seats"].IsSequence()) {
                            r.errors.push_back(prefix + ".seats must be a sequence");
                            r.ok = false;
                        } else {
                            std::size_t si = 0;
                            for (const auto& sn : crew["seats"]) {
                                const std::string sp = prefix + ".seats[" + std::to_string(si) + "]";
                                if (!sn.IsMap()) {
                                    r.errors.push_back(sp + " must be a mapping");
                                    r.ok = false;
                                    ++si;
                                    continue;
                                }
                                MissionCrewSeat mcs;
                                const bool hasSeat = hasKey(sn, "seat");
                                const bool hasRole = hasKey(sn, "role");
                                if (hasSeat == hasRole) {
                                    r.errors.push_back(sp + " must set exactly one of `seat` or `role`");
                                    r.ok = false;
                                }
                                if (hasSeat)
                                    mcs.seatIndex = sn["seat"].as<int>(-1);
                                if (hasRole)
                                    mcs.role = sn["role"].as<std::string>("");
                                if (hasKey(sn, "bot"))
                                    mcs.botSpec = sn["bot"].as<std::string>("");
                                if (hasKey(sn, "skill"))
                                    parseSkill(sn["skill"], sp + ".skill", mcs.skillMin, mcs.skillMax);
                                if (hasKey(sn, "empty"))
                                    mcs.empty = sn["empty"].as<bool>(false);
                                mc.seats.push_back(std::move(mcs));
                                ++si;
                            }
                        }
                    }
                    mo.crew = std::move(mc);
                    if (mo.playerSlot)
                        r.warnings.push_back(prefix + " on a player slot is ignored (a human flies it)");
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

    // ── cameras (optional #910) ─────────────────────────────────────────────────
    // Presentation-only scripted camera shots consumed by the recording client's ShotDirector.
    // The server parses + ignores them. Entity-id cross-refs against `objects` are checked only when
    // object ids are known — an empty knownIds means a cameras-only sidecar doc (--shot-track), which
    // carries no objects; this mirrors the "check refs only when the set is non-empty" sides idiom.
    if (hasKey(doc, "cameras")) {
        const YAML::Node& cams = doc["cameras"];
        if (!cams.IsMap() || !hasKey(cams, "shots")) {
            r.errors.push_back("cameras must be a mapping with a `shots` sequence");
            r.ok = false;
        } else if (!cams["shots"].IsSequence()) {
            r.errors.push_back("cameras.shots must be a sequence");
            r.ok = false;
        } else {
            // Parse a 3-vector node into out[3]; records an error + returns false on a bad shape.
            auto parseVec3 = [&](const YAML::Node& node, const std::string& where, double out[3]) -> bool {
                if (!node.IsSequence() || static_cast<int>(node.size()) != kPosComponents) {
                    r.errors.push_back(where + " must have exactly " + std::to_string(kPosComponents) + " components");
                    r.ok = false;
                    return false;
                }
                for (int c = 0; c < kPosComponents; ++c)
                    out[c] = node[c].as<double>(0.0);
                return true;
            };
            // Cross-check an entity-id ref against objects (skipped for a sidecar with no objects).
            auto checkRef = [&](const std::string& refId, const std::string& where) {
                if (!knownIds.empty() && knownIds.find(refId) == knownIds.end()) {
                    r.errors.push_back(where + " references unknown object id \"" + refId + "\"");
                    r.ok = false;
                }
            };

            std::size_t sidx = 0;
            double prevEnd = 0.0;
            bool havePrev = false;
            for (const auto& sn : cams["shots"]) {
                const std::string sp = "cameras.shots[" + std::to_string(sidx) + "]";
                if (!sn.IsMap()) {
                    r.errors.push_back(sp + " must be a mapping");
                    r.ok = false;
                    ++sidx;
                    continue;
                }
                MissionShot shot;

                // type (required)
                if (!hasKey(sn, "type")) {
                    r.errors.push_back(sp + " missing required field: type");
                    r.ok = false;
                } else {
                    const std::string t = sn["type"].as<std::string>("");
                    if (t == "static")
                        shot.type = ShotType::Static;
                    else if (t == "orbit")
                        shot.type = ShotType::Orbit;
                    else if (t == "chase")
                        shot.type = ShotType::Chase;
                    else if (t == "move")
                        shot.type = ShotType::Move;
                    else {
                        r.errors.push_back(sp + ".type must be static|orbit|chase|move (got \"" + t + "\")");
                        r.ok = false;
                    }
                }

                // start / duration (required)
                bool startValid = false, durationValid = false;
                if (!hasKey(sn, "start")) {
                    r.errors.push_back(sp + " missing required field: start");
                    r.ok = false;
                } else {
                    shot.startSec = sn["start"].as<double>(-1.0);
                    if (shot.startSec < 0.0) {
                        r.errors.push_back(sp + ".start must be >= 0 (got " + std::to_string(shot.startSec) + ")");
                        r.ok = false;
                    } else {
                        startValid = true;
                    }
                }
                if (!hasKey(sn, "duration")) {
                    r.errors.push_back(sp + " missing required field: duration");
                    r.ok = false;
                } else {
                    shot.durationSec = sn["duration"].as<double>(-1.0);
                    if (shot.durationSec <= 0.0) {
                        r.errors.push_back(sp + ".duration must be > 0 (got " + std::to_string(shot.durationSec) + ")");
                        r.ok = false;
                    } else {
                        durationValid = true;
                    }
                }

                // fov (optional; default 60, must be in [20, 120])
                if (hasKey(sn, "fov")) {
                    const float fov = sn["fov"].as<float>(60.f);
                    if (fov < kShotFovMin || fov > kShotFovMax) {
                        r.errors.push_back(sp + ".fov must be in [" + std::to_string(static_cast<int>(kShotFovMin)) +
                                           ", " + std::to_string(static_cast<int>(kShotFovMax)) + "] (got " +
                                           std::to_string(fov) + ")");
                        r.ok = false;
                    }
                    shot.fovYDeg = fov;
                }

                // look_at: entity id or fixed [x,y,z]. Required for static/move; optional (default =
                // target) for orbit/chase.
                bool haveLookAt = false;
                if (hasKey(sn, "look_at")) {
                    const YAML::Node& la = sn["look_at"];
                    if (la.IsScalar()) {
                        shot.lookAtId = la.as<std::string>("");
                        checkRef(shot.lookAtId, sp + ".look_at");
                        haveLookAt = true;
                    } else if (la.IsSequence()) {
                        if (parseVec3(la, sp + ".look_at", shot.lookAtPoint)) {
                            shot.lookAtPointSet = true;
                            haveLookAt = true;
                        }
                    } else {
                        r.errors.push_back(sp + ".look_at must be an object id or a [x, y, z] point");
                        r.ok = false;
                    }
                }

                // per-type fields
                switch (shot.type) {
                case ShotType::Static: {
                    if (!hasKey(sn, "pos")) {
                        r.errors.push_back(sp + " (static) missing required field: pos");
                        r.ok = false;
                    } else {
                        parseVec3(sn["pos"], sp + ".pos", shot.pos);
                    }
                    if (hasKey(sn, "alt"))
                        shot.pos[1] = sn["alt"].as<double>(shot.pos[1]); // MSL override of pos[1]
                    if (!haveLookAt) {
                        r.errors.push_back(sp + " (static) missing required field: look_at");
                        r.ok = false;
                    }
                    break;
                }
                case ShotType::Move: {
                    if (!hasKey(sn, "keyframes") || !sn["keyframes"].IsSequence()) {
                        r.errors.push_back(sp + " (move) missing required field: keyframes (a sequence)");
                        r.ok = false;
                    } else {
                        const YAML::Node& kfs = sn["keyframes"];
                        if (static_cast<int>(kfs.size()) < kMoveKeyframesMinCount) {
                            r.errors.push_back(sp + ".keyframes must have at least " +
                                               std::to_string(kMoveKeyframesMinCount) + " entries");
                            r.ok = false;
                        }
                        std::size_t k = 0;
                        for (const auto& kf : kfs) {
                            const std::string kp = sp + ".keyframes[" + std::to_string(k) + "]";
                            if (!kf.IsMap() || !hasKey(kf, "time") || !hasKey(kf, "pos")) {
                                r.errors.push_back(kp + " must be a mapping with `time` and `pos`");
                                r.ok = false;
                            } else {
                                ShotKeyframe skf;
                                skf.timeSec = kf["time"].as<double>(0.0);
                                if (parseVec3(kf["pos"], kp + ".pos", skf.pos))
                                    shot.keyframes.push_back(skf);
                            }
                            ++k;
                        }
                    }
                    if (hasKey(sn, "ease")) {
                        const std::string e = sn["ease"].as<std::string>("");
                        if (e == "linear")
                            shot.ease = ShotEase::Linear;
                        else if (e == "smooth")
                            shot.ease = ShotEase::Smooth;
                        else {
                            r.errors.push_back(sp + ".ease must be linear|smooth (got \"" + e + "\")");
                            r.ok = false;
                        }
                    }
                    if (!haveLookAt) {
                        r.errors.push_back(sp + " (move) missing required field: look_at");
                        r.ok = false;
                    }
                    break;
                }
                case ShotType::Orbit: {
                    if (!hasKey(sn, "target")) {
                        r.errors.push_back(sp + " (orbit) missing required field: target");
                        r.ok = false;
                    } else {
                        shot.targetId = sn["target"].as<std::string>("");
                        checkRef(shot.targetId, sp + ".target");
                    }
                    if (hasKey(sn, "radius"))
                        shot.orbitRadiusM = sn["radius"].as<double>(shot.orbitRadiusM);
                    if (hasKey(sn, "height"))
                        shot.orbitHeightM = sn["height"].as<double>(shot.orbitHeightM);
                    if (hasKey(sn, "period")) {
                        shot.orbitPeriodSec = sn["period"].as<double>(shot.orbitPeriodSec);
                        if (shot.orbitPeriodSec == 0.0) {
                            r.errors.push_back(sp + ".period must be non-zero (negative = clockwise)");
                            r.ok = false;
                        }
                    }
                    break;
                }
                case ShotType::Chase: {
                    if (!hasKey(sn, "target")) {
                        r.errors.push_back(sp + " (chase) missing required field: target");
                        r.ok = false;
                    } else {
                        shot.targetId = sn["target"].as<std::string>("");
                        checkRef(shot.targetId, sp + ".target");
                    }
                    if (hasKey(sn, "offset"))
                        parseVec3(sn["offset"], sp + ".offset", shot.chaseOffset);
                    if (hasKey(sn, "stiffness")) {
                        shot.chaseStiffness = sn["stiffness"].as<double>(shot.chaseStiffness);
                        if (shot.chaseStiffness < 0.0) {
                            r.errors.push_back(sp + ".stiffness must be >= 0 (0 = rigid)");
                            r.ok = false;
                        }
                    }
                    break;
                }
                }

                // Non-overlap / ascending-order check (in file order — ShotDirector expects sorted).
                if (startValid && durationValid) {
                    if (havePrev && shot.startSec < prevEnd) {
                        r.errors.push_back(sp + ".start (" + std::to_string(shot.startSec) +
                                           ") overlaps or precedes the previous shot's end (" +
                                           std::to_string(prevEnd) + "); shots must be sorted and non-overlapping");
                        r.ok = false;
                    }
                    prevEnd = shot.startSec + shot.durationSec;
                    havePrev = true;
                }

                m.shots.push_back(std::move(shot));
                ++sidx;
            }
        }
    }

    return r;
}

} // namespace fl
