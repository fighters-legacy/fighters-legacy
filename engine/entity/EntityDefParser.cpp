// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/EntityDefParser.h"

#include "config/TomlNumeric.h"
#include "config/TomlRead.h"
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fl {

namespace {

// Reject a key that is not in a section's CLOSED vocabulary (#1106). Only for the sections whose
// field set really is closed — `[signatures]` and `[ai]`. TOML scopes a bare key written after a
// table header INTO that table, so the commonest authoring slip is a key that belongs to `[entity]`
// landing in the section below it; ignoring it silently is how a MiG-29 whose `sensors` suite fell
// into `[signatures]` flies with the builtin eyeball and nothing anywhere says so. A misplaced key
// is an author who meant something, so say what happened rather than dropping it.
void reject_unknown_keys(const toml::table& section, const char* name, std::initializer_list<std::string_view> known) {
    for (const auto& [key, _] : section) {
        const std::string_view k = key.str();
        if (std::find(known.begin(), known.end(), k) == known.end())
            throw std::runtime_error(std::string("unknown key in [") + name + "]: " + std::string(k) +
                                     " — check it is not a key that belongs to an earlier table (a bare "
                                     "key written after a [section] header is scoped into that section)");
    }
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
    if (s == "structure")
        return ObjectCategory::Structure;
    throw std::runtime_error(std::string("unknown category: ") + std::string(s) +
                             " (expected air_vehicle, ground_vehicle, naval_vehicle, "
                             "projectile, effect, player, or structure)");
}

// The optional `projectile_kind` key (#886) — only meaningful on category = "projectile" defs,
// where it selects the builtin placeholder silhouette when the def has no mesh. Absent on a
// projectile defaults to missile (the common case); setting it on any other category is an error
// (the value would be silently dead, which is how content bugs hide).
[[nodiscard]] ProjectileKind parse_projectile_kind(std::string_view s) {
    if (s == "missile")
        return ProjectileKind::Missile;
    if (s == "bomb")
        return ProjectileKind::Bomb;
    if (s == "rocket")
        return ProjectileKind::Rocket;
    throw std::runtime_error(std::string("unknown projectile_kind: ") + std::string(s) +
                             " (expected missile, bomb, or rocket)");
}

// Fixed-length float array (body-frame vectors, quats). Absent = keep the defaults already in
// `out`. Present but not an N-element numeric array is an error.
void parse_float_array(toml::node_view<toml::node> node, float* out, size_t n, const char* field) {
    if (!node)
        return;
    auto* arr = node.as_array();
    if (!arr || arr->size() != n)
        throw std::runtime_error(std::string(field) + " must be an array of " + std::to_string(n) + " numbers");
    size_t i = 0;
    for (auto& el : *arr) {
        auto v = el.value<double>();
        if (!v)
            throw std::runtime_error(std::string(field) + " entries must be numbers");
        out[i++] = static_cast<float>(*v);
    }
}

// Array of non-negative integer station slots. Absent = empty (already the default).
void parse_slot_array(toml::node_view<toml::node> node, std::vector<int>& out, const char* field) {
    if (!node)
        return;
    auto* arr = node.as_array();
    if (!arr)
        throw std::runtime_error(std::string(field) + " must be an array of station slots");
    for (auto& el : *arr) {
        auto v = tomlInt(el);
        if (!v)
            throw std::runtime_error(std::string(field) + " entries must be integer station slots");
        if (*v < 0)
            throw std::runtime_error(std::string(field) + " station slots must be >= 0");
        out.push_back(static_cast<int>(*v));
    }
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

    // projectile_kind (#886): optional, projectile-only; absent projectile = missile.
    if (auto kind_str = opt_string(entity["projectile_kind"]); !kind_str.empty()) {
        if (def.category != ObjectCategory::Projectile)
            throw std::runtime_error("projectile_kind is only valid on category = \"projectile\" (got category = \"" +
                                     cat_str + "\")");
        def.projectileKind = parse_projectile_kind(kind_str);
    } else if (def.category == ObjectCategory::Projectile) {
        def.projectileKind = ProjectileKind::Missile;
    }

    def.maxHp = req_float(entity["max_hp"], "entity.max_hp");
    def.collisionRadiusM = opt_float(entity["collision_radius_m"], 0.f); // 0 = category default (#630)
    def.acceptsLandings = opt_bool(entity["accepts_landings"], false);   // carrier/flight-deck seam (#699)

    // Countermeasure magazines (#529): chaff/flare rounds this entity carries. 0 = none (the default).
    if (auto n = tomlInt(entity["chaff_count"]))
        def.chaffCount = static_cast<uint16_t>(std::clamp<int64_t>(*n, 0, 65535));
    if (auto n = tomlInt(entity["flare_count"]))
        def.flareCount = static_cast<uint16_t>(std::clamp<int64_t>(*n, 0, 65535));
    def.mesh = opt_string(entity["mesh"]);
    def.meshVariant = opt_string(entity["mesh_variant"]);      // optional; empty = untagged nodes only (#882)
    def.cockpitMesh = opt_string(entity["cockpit"]);           // optional; empty = no cockpit geometry (#813)
    def.manualAsset = opt_string(entity["manual"]);            // optional; empty = generated sections only (#821)
    def.flightModelAsset = opt_string(entity["flight_model"]); // optional; empty = the builtin trainer (#1334)
    def.aiScriptAsset = opt_string(entity["ai_script"]);       // optional; empty = no scripted AI

    // Optional progressive damage section
    auto damage_node = tbl["damage"];
    if (damage_node && damage_node.as_table()) {
        DamageDef dmg;
        dmg.light = parse_penalty(damage_node["light"], "light");
        dmg.heavy = parse_penalty(damage_node["heavy"], "heavy");
        dmg.critical = parse_penalty(damage_node["critical"], "critical");

        // Optional per-subsystem granularity (#675): [damage.subsystems] with the fixed vocabulary.
        // Absent = the 3-level model above is the whole story. Any subsystem key may be omitted (its
        // hp stays 0 = not modelled); each present one is { hp, weight }.
        if (auto subs = damage_node["subsystems"]; subs && subs.as_table()) {
            SubsystemSet set;
            auto parse_subsystem = [&](const char* key, Subsystem s) {
                if (auto node = subs[key]; node && node.as_table()) {
                    SubsystemDef sd;
                    sd.hp = opt_float(node["hp"], 0.f);
                    sd.weight = opt_float(node["weight"], 1.f);
                    set.parts[static_cast<int>(s)] = sd;
                }
            };
            parse_subsystem("engine_left", Subsystem::EngineLeft);
            parse_subsystem("engine_right", Subsystem::EngineRight);
            parse_subsystem("engine", Subsystem::Engine); // #901: centreline single engine
            parse_subsystem("controls", Subsystem::Controls);
            parse_subsystem("avionics", Subsystem::Avionics);
            parse_subsystem("hydraulics", Subsystem::Hydraulics);
            parse_subsystem("fuel", Subsystem::Fuel);
            if (set.any())
                dmg.subsystems = set;
        }
        def.damage = std::move(dmg);
    }

    // Optional classic mode section
    auto classic_node = tbl["classic"];
    if (classic_node && classic_node.as_table())
        def.classicDamageMesh = opt_string(classic_node["damage_mesh"]);

    // Optional weapon stations. Authored as an array-of-tables:
    //     [[hardpoints]]
    //     slot = 0
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

            // `type` is ACCEPTED AND IGNORED. Stations used to declare a single kind, which no
            // real multi-role pylon satisfies (bombs OR rockets OR a drop tank on the same wet
            // station); `allowed` already IS the compatibility list, and inert-vs-firing is the
            // mounted WEAPON's kind. The key parses so pre-existing content keeps loading -- the
            // same migration convention as [aircraft].mesh (#813).

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

    // Optional turret mounts (#966/#970). A turret gives a weapon station an aiming frame
    // independent of the airframe nose:
    //     [[turrets]]
    //     id           = "tail"
    //     mount_pos    = [0.0, 0.5, -6.0]
    //     az_min_deg   = -60.0
    //     az_max_deg   =  60.0
    //     el_min_deg   = -10.0
    //     el_max_deg   =  80.0
    //     slew_rate_deg_s = 45.0
    //     stations     = [3]
    // Parsed BEFORE crew so a seat can reference a turret and the partition check sees both.
    if (auto turrets_node = tbl["turrets"]; turrets_node) {
        auto* arr = turrets_node.as_array();
        if (!arr)
            throw std::runtime_error("turrets must be an array of tables ([[turrets]])");
        for (auto& el : *arr) {
            auto* tt = el.as_table();
            if (!tt)
                throw std::runtime_error("turrets must be an array of tables ([[turrets]])");
            TurretDef turret;
            turret.id = req_string((*tt)["id"], "turrets.id");
            parse_float_array((*tt)["mount_pos"], turret.mountPos, 3, "turrets.mount_pos");
            parse_float_array((*tt)["mount_orient"], turret.mountOrient, 4, "turrets.mount_orient");
            turret.azMinDeg = opt_float((*tt)["az_min_deg"], turret.azMinDeg);
            turret.azMaxDeg = opt_float((*tt)["az_max_deg"], turret.azMaxDeg);
            turret.elMinDeg = opt_float((*tt)["el_min_deg"], turret.elMinDeg);
            turret.elMaxDeg = opt_float((*tt)["el_max_deg"], turret.elMaxDeg);
            turret.slewRateDegS = opt_float((*tt)["slew_rate_deg_s"], turret.slewRateDegS);
            parse_slot_array((*tt)["stations"], turret.stations, "turrets.stations");
            def.turrets.push_back(std::move(turret));
        }
    }

    // Optional crew seats (#966/#968). Authored as an array-of-tables; ABSENT = an implicit single
    // pilot seat (the runtime crew frame synthesizes it), so an existing def is unchanged:
    //     [[crew]]
    //     role         = "pilot"
    //     capabilities = ["fly", "radar", "countermeasures"]
    //     [[crew]]
    //     role         = "tail-gunner"
    //     capabilities = ["fire"]
    //     turret       = "tail"
    //     bot          = "gunner"
    //     skill        = 0.6
    // Roles are display strings; the engine sees only the capability mask (roles-as-data, #944).
    if (auto crew_node = tbl["crew"]; crew_node) {
        auto* arr = crew_node.as_array();
        if (!arr)
            throw std::runtime_error("crew must be an array of tables ([[crew]])");
        for (auto& el : *arr) {
            auto* ct = el.as_table();
            if (!ct)
                throw std::runtime_error("crew must be an array of tables ([[crew]])");
            SeatDef seat;
            seat.role = req_string((*ct)["role"], "crew.role");

            auto* caps = (*ct)["capabilities"].as_array();
            if (!caps || caps->empty())
                throw std::runtime_error("crew.capabilities must be a non-empty array (seat \"" + seat.role + "\")");
            for (auto& c : *caps) {
                auto tok = c.value<std::string>();
                if (!tok || tok->empty())
                    throw std::runtime_error("crew.capabilities entries must be non-empty strings");
                auto cap = parseCrewCapability(*tok);
                if (!cap)
                    throw std::runtime_error("unknown crew capability \"" + *tok +
                                             "\" (expected fly, fire, radar, countermeasures, command)");
                seat.capabilities = withCapability(seat.capabilities, *cap);
            }

            parse_slot_array((*ct)["stations"], seat.stations, "crew.stations");
            seat.turret = opt_string((*ct)["turret"]);
            parse_float_array((*ct)["seat_pos"], seat.eyepoint, 3, "crew.seat_pos");

            // Default occupancy: `bot = "<spec>"` OR `empty = true`. Human is a runtime state.
            auto botSpec = opt_string((*ct)["bot"]);
            const bool emptyFlag = opt_bool((*ct)["empty"], false);
            if (emptyFlag && !botSpec.empty())
                throw std::runtime_error("crew seat \"" + seat.role + "\" declares both bot and empty");
            if (emptyFlag) {
                seat.defaultOccupancy = SeatOccupancyDefault::Empty;
            } else {
                seat.defaultOccupancy = SeatOccupancyDefault::Bot;
                seat.botSpec = std::move(botSpec);
            }
            seat.defaultSkill = parse_unit_fraction((*ct)["skill"], "crew.skill", seat.defaultSkill);

            // Crew-seat damage (#978): an optional independent HP pool + hit-bias weight. Absent = a
            // non-damageable seat (damageHp 0), preserving the #675 fallback.
            seat.damageHp = opt_float((*ct)["damage_hp"], 0.f);
            if (seat.damageHp < 0.f)
                throw std::runtime_error("crew.damage_hp must be >= 0 (seat \"" + seat.role + "\")");
            seat.hitWeight = opt_float((*ct)["hit_weight"], 1.f);
            if (seat.hitWeight <= 0.f)
                throw std::runtime_error("crew.hit_weight must be > 0 (seat \"" + seat.role + "\")");

            def.crew.push_back(std::move(seat));
        }
    }

    // One-owner-per-channel invariant (#966): exactly one Fly seat, each hardpoint/turret/Radar/
    // Countermeasures/Command owned by at most one seat. A single-file structural check, so it
    // lives in the shared parser (the engine enforces it, and validate-entity surfaces the throw).
    {
        std::vector<int> hardpointSlots;
        hardpointSlots.reserve(def.hardpoints.size());
        for (const auto& hp : def.hardpoints)
            hardpointSlots.push_back(hp.slot);
        if (std::string err = validateCrewPartition(def.crew, def.turrets, hardpointSlots); !err.empty())
            throw std::runtime_error("crew partition invalid: " + err);
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
        reject_unknown_keys(*sig_node.as_table(), "signatures", {"rcs", "ir", "visual", "laser"});
        def.signatures.rcs = parse_signature(sig_node["rcs"], "signatures.rcs", def.signatures.rcs);
        def.signatures.ir = parse_signature(sig_node["ir"], "signatures.ir", def.signatures.ir);
        def.signatures.visual = parse_signature(sig_node["visual"], "signatures.visual", def.signatures.visual);
        def.signatures.laser = parse_signature(sig_node["laser"], "signatures.laser", def.signatures.laser);
    }

    // Optional per-unit AI tuning.
    if (auto ai_node = tbl["ai"]; ai_node && ai_node.as_table()) {
        reject_unknown_keys(*ai_node.as_table(), "ai", {"skill", "reaction"});
        AiTuning tuning;
        tuning.skill = parse_unit_fraction(ai_node["skill"], "ai.skill", tuning.skill);
        tuning.reaction = parse_unit_fraction(ai_node["reaction"], "ai.reaction", tuning.reaction);
        def.aiTuning = tuning;
    }

    // Optional flight deck (#38): footprint + catapult + arrest-wire geometry, ship-local metres.
    // Meaningful only with accepts_landings — a deck nothing may land on is a modelling error worth
    // failing loudly rather than silently ignoring.
    if (auto deck_node = tbl["deck"]; deck_node && deck_node.as_table()) {
        if (!def.acceptsLandings)
            throw std::runtime_error("[deck] requires accepts_landings = true (a deck nothing may land on)");
        DeckDef deck;
        deck.lengthM = req_float(deck_node["length_m"], "deck.length_m");
        deck.widthM = req_float(deck_node["width_m"], "deck.width_m");
        deck.heightM = req_float(deck_node["height_m"], "deck.height_m");
        if (deck.lengthM <= 0.f || deck.widthM <= 0.f || deck.heightM < 0.f)
            throw std::runtime_error("[deck] length_m/width_m must be > 0 and height_m >= 0");
        deck.catStartXM = opt_float(deck_node["cat_start_x_m"], deck.catStartXM);
        deck.catStrokeM = opt_float(deck_node["cat_stroke_m"], deck.catStrokeM);
        deck.catEndSpeedMps = opt_float(deck_node["cat_end_speed_mps"], deck.catEndSpeedMps);
        deck.wireXM = opt_float(deck_node["wire_x_m"], deck.wireXM);
        deck.wireZoneM = opt_float(deck_node["wire_zone_m"], deck.wireZoneM);
        deck.maxTrapSpeedMps = opt_float(deck_node["max_trap_speed_mps"], deck.maxTrapSpeedMps);
        if (deck.catStrokeM <= 0.f || deck.catEndSpeedMps <= 0.f || deck.wireZoneM <= 0.f ||
            deck.maxTrapSpeedMps <= 0.f)
            throw std::runtime_error("[deck] catapult/wire parameters must be > 0");
        def.deck = deck;
    }

    return def;
}

} // namespace fl
