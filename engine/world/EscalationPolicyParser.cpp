// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/EscalationPolicyParser.h"

#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>

namespace fl {

namespace {

[[nodiscard]] std::string req_string(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<std::string>();
    if (!v)
        throw std::runtime_error(std::string("zone policy: missing required field: ") + field);
    return std::move(*v);
}

// Dwell values are authored as seconds and may legitimately be written as integers (45) or floats
// (4.5), so read through toml++'s numeric coercion rather than requiring one spelling.
[[nodiscard]] double dwell(const toml::node* node, const std::string& field, double fallback) {
    if (!node)
        return fallback;
    auto v = node->value<double>();
    if (!v)
        throw std::runtime_error("zone policy: " + field + " must be a number of seconds");
    if (*v < 0.0)
        throw std::runtime_error("zone policy: " + field + " must not be negative");
    return *v;
}

void parseLevelRow(const toml::table& tbl, AlertLevel level, EscalationDwell& out) {
    const std::string section = std::string("escalation.") + std::string(alertLevelName(level));

    // A row that names no thresholds keeps the struct defaults, so an author writing an empty
    // [escalation.war_state] gets exactly the all-zero war-state posture the schema documents.
    out.warningDwellS = dwell(tbl.get("warning_dwell"), section + ".warning_dwell", out.warningDwellS);
    out.interceptDwellS = dwell(tbl.get("intercept_dwell"), section + ".intercept_dwell", out.interceptDwellS);
    out.hostileDwellS = dwell(tbl.get("hostile_dwell"), section + ".hostile_dwell", out.hostileDwellS);
    out.complianceCooldownS =
        dwell(tbl.get("compliance_cooldown"), section + ".compliance_cooldown", out.complianceCooldownS);

    if (const auto* reset = tbl.get("compliance_reset")) {
        auto b = reset->value<bool>();
        if (!b)
            throw std::runtime_error(section + ".compliance_reset must be a boolean");
        out.complianceReset = *b;
    }

    // Thresholds are cumulative from zone entry, so a later stage that fires earlier than an
    // earlier one is unreachable -- and silently unreachable escalation is exactly the kind of
    // authoring slip nobody notices until a mission plays wrong.
    if (out.interceptDwellS < out.warningDwellS || out.hostileDwellS < out.interceptDwellS)
        throw std::runtime_error(section + ": dwell thresholds must not decrease (warning <= intercept <= hostile)");
}

} // namespace

EscalationPolicy parseEscalationPolicy(std::string_view toml) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("zone policy: TOML parse error: ") + e.description().data());
    }

    auto policy = tbl["policy"];
    if (!policy.is_table())
        throw std::runtime_error("zone policy: missing [policy] table");

    EscalationPolicy out;
    out.id = req_string(policy["id"], "policy.id");
    out.name = req_string(policy["name"], "policy.name");

    auto escalation = tbl["escalation"];
    if (escalation && !escalation.is_table())
        throw std::runtime_error("zone policy: [escalation] must be a table");

    if (auto* escTbl = escalation.as_table()) {
        // Reject unknown level sections rather than ignoring them: a typo'd [escalation.wartime]
        // would otherwise leave the real war-state row at its defaults, which reads as the engine
        // ignoring the author.
        for (const auto& [key, node] : *escTbl) {
            AlertLevel level{};
            if (!alertLevelFromString(std::string_view{key.str()}, level))
                throw std::runtime_error("zone policy: unknown alert level section [escalation." +
                                         std::string(key.str()) + "]");
            if (!node.is_table())
                throw std::runtime_error("zone policy: [escalation." + std::string(key.str()) + "] must be a table");
            parseLevelRow(*node.as_table(), level, out.byLevel[static_cast<std::size_t>(level)]);
        }
    }

    return out;
}

} // namespace fl
