// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/EntityDefParser.h"

#include <toml++/toml.hpp>

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

    return def;
}

} // namespace fl
