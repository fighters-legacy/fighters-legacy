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
// wingman, versus two builtin-fighter bandits, a crewed builtin:bomber (its bot tail-gunner turret
// auto-defends -- the #977 zero-pack crew/turret proof), and a SAM site defending a point. Win by
// destroying the SAM; a generous timer backstop fails the mission so a headless harness always ends.
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
  # A crewed bomber (#977): its AI pilot flies, and its bot tail-gunner turret auto-defends against
  # anyone who slides into its rear quarter -- the whole crew/turret fire path, zero-pack.
  - { type: builtin:bomber, id: redbomber, side: red, pos: [9500, 3000, -400], heading: 270, ai: "lua builtin:fighter" }
  - { type: builtin:sam-site, id: redsam, side: red, pos: [9500, 0, 0], heading: 90, ai: "sam" }
triggers:
  - on: destroy(redsam)
    do: mission_success
  - on: timer(600)
    do: mission_failure
)yaml";

inline constexpr std::string_view kBuiltinSandboxId = "builtin:sandbox";

// builtin:shape-gallery -- the visual-verification scene for the per-category builtin placeholder
// meshes (#886): a museum row with one of every surface category plus floating ordnance exhibits
// (projectile entity types spawned as plain objects -- no ai/route/default script means no
// controller and no integrator, so they hold their spawn transform), a joinable armed player slot
// for firing bombs/rockets and making wrecks by hand, and a live fight 9 km out (fighters + SAM +
// AAA) so missile silhouettes appear in flight. Launch via `fighters-legacy --mission
// builtin:shape-gallery` or `tools/visual_check.sh`.
inline constexpr std::string_view kBuiltinShapeGalleryYaml = R"yaml(
name: "Shape Gallery"
map: world
layer: world_clear
time: { hour: 12, minute: 0 }
wind: { heading: 250, speed: 2 }
sides: [blue, red]
objects:
  # Joinable armed player slot overlooking the museum row (all 8 stations; fire bombs/rockets here).
  - { type: builtin:debug-entity, id: player, side: blue, pos: [-800, 1200, 0], heading: 90, player: true }
  # Escort orbiting the gallery: a moving air-vehicle silhouette that engages when the reds arrive.
  - { type: builtin:debug-entity, id: escort, side: blue, pos: [-400, 1400, 300], heading: 90, ai: "lua builtin:fighter" }
  # Museum row: one of every surface category, parked on the terrain.
  - { type: builtin:ground-vehicle, id: gv, side: blue, pos: [600, 0, -240], heading: 0, start: ground }
  - { type: builtin:static-target, id: bunker, side: blue, pos: [600, 0, -120], heading: 0, start: ground }
  - { type: builtin:naval-vessel, id: ship, side: blue, pos: [700, 0, 220], heading: 0, start: ground }
  # Ordnance exhibits: projectile entity types floating in a line above the row.
  - { type: "projectile:builtin:ir-missile", id: exhibit-missile, side: blue, pos: [600, 0, -40], heading: 90, alt: 620 }
  - { type: "projectile:builtin:bomb", id: exhibit-bomb, side: blue, pos: [600, 0, 0], heading: 90, alt: 620 }
  - { type: "projectile:builtin:rocket", id: exhibit-rocket, side: blue, pos: [600, 0, 40], heading: 90, alt: 620 }
  # Live fire 9 km out (strays stay far from the museum): bandits + a SAM + AAA.
  - { type: builtin:debug-entity, id: red1, side: red, pos: [9000, 2500, 0], heading: 270, ai: "lua builtin:fighter" }
  - { type: builtin:debug-entity, id: red2, side: red, pos: [9000, 2500, 900], heading: 270, ai: "lua builtin:fighter" }
  - { type: builtin:sam-site, id: redsam, side: red, pos: [9500, 0, 0], heading: 90, start: ground, ai: "sam" }
  - { type: builtin:aaa, id: redaaa, side: red, pos: [9200, 0, 500], heading: 90, start: ground, ai: "aaa" }
triggers:
  - on: destroy(redsam)
    do: mission_success
  - on: timer(3600)
    do: mission_failure
)yaml";

inline constexpr std::string_view kBuiltinShapeGalleryId = "builtin:shape-gallery";

} // namespace detail

// The YAML for a builtin mission id ("builtin:sandbox"), or empty if unknown. fl-server checks this
// FIRST when resolving a mission to load (the "builtin:" namespace cannot collide with a pack id), so
// `--mission builtin:sandbox` and a `[rotation]` entry of "builtin:sandbox" both work zero-pack.
[[nodiscard]] inline std::string_view builtinMissionYaml(std::string_view id) noexcept {
    if (id == detail::kBuiltinSandboxId)
        return detail::kBuiltinSandboxYaml;
    if (id == detail::kBuiltinShapeGalleryId)
        return detail::kBuiltinShapeGalleryYaml;
    return {};
}

// Every builtin mission id, for listing.
[[nodiscard]] inline std::span<const std::string_view> builtinMissionIds() noexcept {
    static constexpr std::string_view kIds[] = {detail::kBuiltinSandboxId, detail::kBuiltinShapeGalleryId};
    return kIds;
}

} // namespace fl
