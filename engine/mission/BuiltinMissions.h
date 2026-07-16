// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string_view>

// Compiled-in missions for the zero-content-pack sandbox (#868), mirroring BuiltinWeapon /
// BuiltinEntities / BuiltinAiScripts: the #632 mission runtime (objectives, triggers, scripted bots)
// must be exercisable with no pack mounted, and Instant Action (#40) needs something to launch. The
// YAML is parsed by the SAME parseMission() the pack path uses, so a builtin mission and a pack
// mission share one schema. It references only builtin content (builtin:debug-entity + builtin:sam-site,
// the builtin:fighter Lua AI, the `sam` behavior), so it resolves from a bare checkout.
namespace fl {

namespace detail {

// builtin:sandbox -- a small skirmish: a joinable blue player slot flanked by a builtin-fighter
// wingman, versus two builtin-fighter bandits and a SAM site defending a point. Win by destroying the
// SAM; a generous timer backstop fails the mission so a headless harness run always terminates.
inline constexpr std::string_view kBuiltinSandboxYaml = R"yaml(
name: "Sandbox Skirmish"
map: world
layer: world_clear
time: { hour: 12, minute: 0 }
wind: { heading: 250, speed: 4 }
sides: [blue, red]
objects:
  - { type: builtin:debug-entity, id: player, side: blue, pos: [0, 1500, 0], heading: 90, player: true }
  - { type: builtin:debug-entity, id: blue1, side: blue, pos: [-600, 1500, 200], heading: 90, ai: "lua builtin:fighter" }
  - { type: builtin:debug-entity, id: red1, side: red, pos: [9000, 2500, 0], heading: 270, ai: "lua builtin:fighter" }
  - { type: builtin:debug-entity, id: red2, side: red, pos: [9000, 2500, 900], heading: 270, ai: "lua builtin:fighter" }
  - { type: builtin:sam-site, id: redsam, side: red, pos: [9500, 0, 0], heading: 90, ai: "sam" }
triggers:
  - on: destroy(redsam)
    do: mission_success
  - on: timer(600)
    do: mission_failure
)yaml";

inline constexpr std::string_view kBuiltinSandboxId = "builtin:sandbox";

} // namespace detail

// The YAML for a builtin mission id ("builtin:sandbox"), or empty if unknown. fl-server checks this
// FIRST when resolving a mission to load (the "builtin:" namespace cannot collide with a pack id), so
// `--mission builtin:sandbox` and a `[rotation]` entry of "builtin:sandbox" both work zero-pack.
[[nodiscard]] inline std::string_view builtinMissionYaml(std::string_view id) noexcept {
    if (id == detail::kBuiltinSandboxId)
        return detail::kBuiltinSandboxYaml;
    return {};
}

// Every builtin mission id, for listing.
[[nodiscard]] inline std::span<const std::string_view> builtinMissionIds() noexcept {
    static constexpr std::string_view kIds[] = {detail::kBuiltinSandboxId};
    return kIds;
}

} // namespace fl
