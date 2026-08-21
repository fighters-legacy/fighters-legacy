// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fl {

// The ONE `name(<arg>)` trigger-ref grammar (#1239). MissionParser used a regex and MissionRuntime
// a hand-rolled prefix/suffix split, and they disagreed on malformed refs: `destroy(sam1))` matched
// neither the validator's regex (so the unknown-id check silently never ran and validation passed)
// nor anything the runtime could fire — a trigger that validates but can never happen. One parser,
// used by both, makes "validate-mission accepts it" and "the runtime can fire it" the same set.
//
// Strict shape: `name(` + a non-empty arg containing no parentheses + `)` ending the string.
[[nodiscard]] inline std::optional<std::string> triggerArg(std::string_view s, std::string_view name) {
    if (s.size() < name.size() + 3 || s.substr(0, name.size()) != name || s[name.size()] != '(')
        return std::nullopt;
    if (s.back() != ')')
        return std::nullopt;
    const std::string_view arg = s.substr(name.size() + 1, s.size() - name.size() - 2);
    if (arg.empty() || arg.find_first_of("()") != std::string_view::npos)
        return std::nullopt;
    return std::string(arg);
}

// True when `s` starts like a `name(` ref at all — what lets the validator tell "malformed
// destroy/timer ref" (an error: the runtime can never fire it) from an unrelated predicate
// (legal: reach/zone extensions and Lua-only predicates never fire here by design).
[[nodiscard]] inline bool looksLikeTriggerRef(std::string_view s, std::string_view name) {
    return s.size() > name.size() && s.substr(0, name.size()) == name && s[name.size()] == '(';
}

} // namespace fl
