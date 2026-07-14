// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/EntityDefParser.h"

#include "config/TomlNumeric.h"
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fl {

namespace {

// ── helpers ──────────────────────────────────────────────────────────────────

[[nodiscard]] std::string req_string(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<std::string>();
    if (!v)
        throw std::runtime_error(std::string("missing required field: ") + field);
    return std::move(*v);
}

[[nodiscard]] float req_float(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<double>();
    if (!v)
        throw std::runtime_error(std::string("missing required field: ") + field);
    return static_cast<float>(*v);
}

[[nodiscard]] float opt_float(toml::node_view<toml::node> node, float fallback) {
    auto v = node.value<double>();
    return v ? static_cast<float>(*v) : fallback;
}

[[nodiscard]] bool opt_bool(toml::node_view<toml::node> node, bool fallback) {
    auto v = node.value<bool>();
    return v ? *v : fallback;
}

[[nodiscard]] std::string opt_string(toml::node_view<toml::node> node) {
    auto v = node.value<std::string>();
    return v ? std::move(*v) : std::string{};
}

// A signature multiplier: absent keeps the baseline the caller passed in. Zero is rejected along
// with the negatives — a signature of 0 is not "very stealthy", it is a target no sensor of that
// type can ever detect at any range, and an author who wants that should say so with a number.
[[nodiscard]] float parse_signature(toml::node_view<toml::node> node, const char* field, float fallback) {
    auto v = node.value<double>();
    if (!v)
        return fallback;
    const auto value = static_cast<float>(*v);
    if (value <= 0.f || value > 100.f)
        throw std::runtime_error(std::string(field) + " must be in (0, 100]");
    return value;
}

[[nodiscard]] float parse_unit_fraction(toml::node_view<toml::node> node, const char* field, float fallback) {
    auto v = node.value<double>();
    if (!v)
        return fallback;
    const auto value = static_cast<float>(*v);
    if (value < 0.f || value > 1.f)
        throw std::runtime_error(std::string(field) + " must be in [0, 1]");
    return value;
}

[[nodiscard]] ObjectCategory parse_category(std::string_view s) {
    if (s == "air_vehicle")
        return ObjectCategory::AirVehicle;
    if (s == "ground_vehicle")
        return ObjectCategory::GroundVehicle;
    if (s == "naval_vehicle")
        return ObjectCategory::NavalVehicle;
    if (s == "projectile")
        return ObjectCategory::Projectile;
    if (s == "effect")
        return ObjectCategory::Effect;
    if (s == "player")
        return ObjectCategory::Player;
    throw std::runtime_error(std::string("unknown category: ") + std::string(s) +
                             " (expected air_vehicle, ground_vehicle, naval_vehicle, "
                             "projectile, effect, or player)");
}

[[nodiscard]] HardpointType parse_hardpoint_type(std::string_view s) {
    if (s == "missile")
        return HardpointType::Missile;
    if (s == "bomb")
        return HardpointType::Bomb;
    if (s == "rocket")
        return HardpointType::Rocket;
    if (s == "gun")
        return HardpointType::Gun;
    if (s == "fuel")
        return HardpointType::Fuel;
    if (s == "pod")
        return HardpointType::Pod;
    throw std::runtime_error(std::string("unknown hardpoint type: ") + std::string(s) +
                             " (expected missile, bomb, rocket, gun, fuel, or pod)");
}

[[nodiscard]] DamagePenalty parse_penalty(toml::node_view<toml::node> node, const char* name) {
    auto* tbl = node.as_table();
    if (!tbl)
        throw std::runtime_error(std::string("missing required damage section: [damage.") + name + "]");

    DamagePenalty p;
    p.hpFraction = req_float((*tbl)["hp_fraction"], (std::string("damage.") + name + ".hp_fraction").c_str());
    if (p.hpFraction <= 0.f || p.hpFraction > 1.f)
        throw std::runtime_error(std::string("damage.") + name + ".hp_fraction must be in (0, 1]");
    p.visualEffect = opt_string((*tbl)["visual_effect"]);
    p.thrustFactor = opt_float((*tbl)["thrust_factor"], 1.f);
    p.controlFactor = opt_float((*tbl)["control_factor"], 1.f);
    p.avionicsFailure = opt_bool((*tbl)["avionics_failure"], false);
    return p;
}

} // namespace

// ── public API ────────────────────────────────────────────────────────────────

EntityDef parseEntityDef(std::string_view toml_src) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_src);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("entity def parse error: ") + e.what());
    }

    auto entity = tbl["entity"];
    if (!entity)
        throw std::runtime_error("missing required table [entity]");

    EntityDef def;
    def.id = req_string(entity["id"], "entity.id");
    def.name = req_string(entity["name"], "entity.name");

    auto cat_str = req_string(entity["category"], "entity.category");
    def.category = parse_category(cat_str);

    def.maxHp = req_float(entity["max_hp"], "entity.max_hp");
    def.mesh = opt_string(entity["mesh"]);
    def.cockpitMesh = opt_string(entity["cockpit"]);           // optional; empty = no cockpit geometry (#813)
    def.manualAsset = opt_string(entity["manual"]);            // optional; empty = generated sections only (#821)
    def.flightModelAsset = opt_string(entity["flight_model"]); // optional; empty = builtin UFO model
    def.aiScriptAsset = opt_string(entity["ai_script"]);       // optional; empty = no scripted AI

    // Optional progressive damage section
    auto damage_node = tbl["damage"];
    if (damage_node && damage_node.as_table()) {
        DamageDef dmg;
        dmg.light = parse_penalty(damage_node["light"], "light");
        dmg.heavy = parse_penalty(damage_node["heavy"], "heavy");
        dmg.critical = parse_penalty(damage_node["critical"], "critical");
        def.damage = std::move(dmg);
    }

    // Optional classic mode section
    auto classic_node = tbl["classic"];
    if (classic_node && classic_node.as_table())
        def.classicDamageMesh = opt_string(classic_node["damage_mesh"]);

    // Optional weapon stations. Authored as an array-of-tables:
    //     [[hardpoints]]
    //     slot = 0
    //     type = "missile"
    //     allowed = ["aim120c", "aim9x"]
    //     default = "aim120c"
    if (auto hp_node = tbl["hardpoints"]; hp_node) {
        auto* arr = hp_node.as_array();
        if (!arr)
            throw std::runtime_error("hardpoints must be an array of tables ([[hardpoints]])");

        for (auto& el : *arr) {
            auto* hp_tbl = el.as_table();
            if (!hp_tbl)
                throw std::runtime_error("hardpoints must be an array of tables ([[hardpoints]])");

            Hardpoint hp;
            auto slot = tomlInt((*hp_tbl)["slot"]);
            if (!slot)
                throw std::runtime_error("missing required field: hardpoints.slot");
            if (*slot < 0)
                throw std::runtime_error("hardpoints.slot must be >= 0");
            hp.slot = static_cast<int>(*slot);

            for (const auto& existing : def.hardpoints) {
                if (existing.slot == hp.slot)
                    throw std::runtime_error("duplicate hardpoints.slot: " + std::to_string(hp.slot));
            }

            hp.type = parse_hardpoint_type(req_string((*hp_tbl)["type"], "hardpoints.type"));

            auto* allowed = (*hp_tbl)["allowed"].as_array();
            if (!allowed || allowed->empty())
                throw std::runtime_error("hardpoints.allowed must be a non-empty array of weapon ids");
            for (auto& a : *allowed) {
                auto id = a.value<std::string>();
                if (!id || id->empty())
                    throw std::runtime_error("hardpoints.allowed entries must be non-empty strings");
                hp.allowed.push_back(std::move(*id));
            }

            // AN EMPTY STATION IS A LEGITIMATE LOADOUT CHOICE, and `default = ""` is how content
            // says so. Loadout::defaultPayload already treats it that way and says as much in its
            // own comment -- but the parser used to throw on it, so the entity never survived to
            // reach that code. The two halves of the engine disagreed, and the parser won.
            //
            // This is not a corner case: it is the NORMAL case. An F-5E's published performance is
            // flown with its pylons CLEAN, and every aircraft that spawns without a full war load
            // needs to express "this station exists, it can carry these things, and right now it is
            // empty". Requiring a default store on every station makes that unsayable.
            hp.defaultWeapon = req_string((*hp_tbl)["default"], "hardpoints.default");
            if (!hp.defaultWeapon.empty() &&
                std::find(hp.allowed.begin(), hp.allowed.end(), hp.defaultWeapon) == hp.allowed.end())
                throw std::runtime_error("hardpoints.default \"" + hp.defaultWeapon +
                                         "\" is not listed in hardpoints.allowed");

            def.hardpoints.push_back(std::move(hp));
        }
    }

    // Optional sensor suite: sensor-def ids the entity carries.
    //
    //     [entity]
    //     sensors = ["fl-base:eyeball", "fl-base:apg63"]
    //
    // Unknown ids are NOT rejected here: a pack's cross-references resolve once every file in it has
    // been read, so the check belongs at resolve time (a warning), not in a single-file parser. What
    // IS rejected is a list that cannot mean anything — a non-array, an empty id, a duplicate.
    if (auto sensors_node = entity["sensors"]; sensors_node) {
        auto* arr = sensors_node.as_array();
        if (!arr)
            throw std::runtime_error("entity.sensors must be an array of sensor ids");

        for (auto& el : *arr) {
            auto id = el.value<std::string>();
            if (!id || id->empty())
                throw std::runtime_error("entity.sensors entries must be non-empty strings");
            if (std::find(def.sensorIds.begin(), def.sensorIds.end(), *id) != def.sensorIds.end())
                throw std::runtime_error("duplicate entity.sensors id: " + *id);
            def.sensorIds.push_back(std::move(*id));
        }
    }

    // Optional signature section. Unitless multipliers against a baseline fighter (1.0); absent
    // fields keep the baseline, so a pack tunes only what differs.
    if (auto sig_node = tbl["signatures"]; sig_node && sig_node.as_table()) {
        def.signatures.rcs = parse_signature(sig_node["rcs"], "signatures.rcs", def.signatures.rcs);
        def.signatures.ir = parse_signature(sig_node["ir"], "signatures.ir", def.signatures.ir);
        def.signatures.visual = parse_signature(sig_node["visual"], "signatures.visual", def.signatures.visual);
        def.signatures.laser = parse_signature(sig_node["laser"], "signatures.laser", def.signatures.laser);
    }

    // Optional per-unit AI tuning.
    if (auto ai_node = tbl["ai"]; ai_node && ai_node.as_table()) {
        AiTuning tuning;
        tuning.skill = parse_unit_fraction(ai_node["skill"], "ai.skill", tuning.skill);
        tuning.reaction = parse_unit_fraction(ai_node["reaction"], "ai.reaction", tuning.reaction);
        def.aiTuning = tuning;
    }

    return def;
}

} // namespace fl
