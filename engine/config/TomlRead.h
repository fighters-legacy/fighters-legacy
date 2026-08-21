// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace fl {

// Required/optional field reads for the TOML def parsers, and the one place their error text is
// written (#1245). Seven parsers — entity, weapon, sensor, flight model, airport, escalation
// policy, livery — each carried its own copy of these four or five functions; the copies were
// uniform in behaviour and had drifted only in spelling, which is exactly the kind of duplication
// that makes a content author's error message depend on which parser happened to reject the file.
//
// Message shape, for every required read:  <prefix>missing required field: <field>
// `prefix` is the parser's own context ("airport: ", "zone policy: ", "livery def parse error: ")
// and defaults to empty for the parsers that never had one. It is the caller's job to pass the
// same prefix at every call in a file — bind it to a file-local constant rather than repeating the
// literal.
//
// Integer fields do NOT belong here: read those with TomlNumeric.h, which exists for the float→int
// UB in toml++ (#824). These helpers only take the safe double/string/bool path.
//
// Include this from .cpp files of targets that link tomlplusplus, never from a public header —
// the same rule TomlNumeric.h obeys, which is what keeps tomlplusplus PRIVATE on every target.

namespace detail {

[[noreturn]] inline void tomlMissing(const char* prefix, const char* field) {
    throw std::runtime_error(std::string(prefix) + "missing required field: " + field);
}

} // namespace detail

[[nodiscard]] inline std::string req_string(toml::node_view<toml::node> node, const char* field,
                                            const char* prefix = "") {
    auto v = node.value<std::string>();
    if (!v)
        detail::tomlMissing(prefix, field);
    return std::move(*v);
}

[[nodiscard]] inline double req_double(toml::node_view<toml::node> node, const char* field, const char* prefix = "") {
    auto v = node.value<double>();
    if (!v)
        detail::tomlMissing(prefix, field);
    return *v;
}

[[nodiscard]] inline float req_float(toml::node_view<toml::node> node, const char* field, const char* prefix = "") {
    return static_cast<float>(req_double(node, field, prefix));
}

// A required array of numbers. Its two failure modes read differently from a missing scalar because
// an empty array and a non-numeric element are distinct authoring mistakes.
[[nodiscard]] inline std::vector<float> req_float_array(toml::node_view<toml::node> node, const char* field,
                                                        const char* prefix = "") {
    auto* arr = node.as_array();
    if (!arr || arr->empty())
        throw std::runtime_error(std::string(prefix) + "missing or empty required array: " + field);
    std::vector<float> out;
    out.reserve(arr->size());
    for (auto& el : *arr) {
        auto v = el.value<double>();
        if (!v)
            throw std::runtime_error(std::string(prefix) + "non-numeric value in array: " + field);
        out.push_back(static_cast<float>(*v));
    }
    return out;
}

[[nodiscard]] inline std::string opt_string(toml::node_view<toml::node> node) {
    auto v = node.value<std::string>();
    return v ? std::move(*v) : std::string{};
}

[[nodiscard]] inline float opt_float(toml::node_view<toml::node> node, float fallback) {
    auto v = node.value<double>();
    return v ? static_cast<float>(*v) : fallback;
}

[[nodiscard]] inline bool opt_bool(toml::node_view<toml::node> node, bool fallback) {
    auto v = node.value<bool>();
    return v ? *v : fallback;
}

} // namespace fl
