// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Server role / permission vocabulary (#945, epic #944).
//
// A graded server authority model built on a capability BITMASK plus named role presets, NOT a flat
// role enum. Rationale (decision record on #944):
//   1. Per-command granularity is needed anyway for the Epic M (#589) agent/MCP command allowlist —
//      humans and agents flow through one gate.
//   2. A new role is a new preset with no wire churn: self-host communities eventually define their
//      own presets in fl-server.toml.
//   3. A faction leader needs a faction parameter, which a parameterized grant (PeerAuthority) carries
//      naturally alongside the mask.
//
// This header is stdlib-only (the WingmanCommand.h pattern), so engine-net, fl-server, and the game
// client include it with no link dependency. It is pure vocabulary: nothing here enforces anything.
//
// ORTHOGONALITY INVARIANT: authority (this mask) is SEPARATE from PeerRole (Pilot/Observer, which is
// embodiment only). A game master is typically an Observer holding GmMap caps; a faction leader a
// Pilot holding faction caps. Any combination is legal. PeerRole never implies a capability and a
// capability never implies a role.

namespace fl {

// Capability bit positions. Each names one class of privileged action; a CapabilityMask is a set of
// these bits. The initial vocabulary is fixed by the #944 epic; new bits are appended before Count
// (never reordered — a bit position is a stable wire-facing meaning once the mask crosses ConnectAck).
enum class Capability : uint8_t {
    KickBan = 0,          // kick / ban / unban peers
    Mute = 1,             // mute / unmute chat. LIVE (not reserved): text chat + the mute command
                          // landed in #646/#1003, ahead of the #944 issue text that called it reserved.
    ServerConfig = 2,     // set_weather / set_time / reload_config / shutdown / pause / resume / trace
    SpawnAny = 3,         // spawn / kill / tp / detonate / respawn any entity, any faction
    SpawnFaction = 4,     // spawn/kill for the grant's own faction only (#948 enforces the scoping)
    CommandAnyAi = 5,     // issue flight/formation orders (#610) to any AI, any faction
    CommandFactionAi = 6, // flight/formation orders for the grant's own faction only (#948)
    FactionPosture = 7,   // set FactionRegistry alert levels
    GmMap = 8,            // receive + drive the game-master overview map (#861), full battlespace
    GmMapFaction = 9,     // the faction-filtered GM map (#948): only the grant faction's picture
    SpectateAny = 10,     // spectate any entity's view (the #403 spectate command)
    GrantRoles = 11,      // grant / revoke authority to other peers (the grant ladder, #947)

    Count = 12,
};

// A set of capabilities. uint64_t leaves ample headroom (12 bits used) for community-defined presets
// without a wire-width change; the granted-authority ConnectAck TLV (#949) carries it as a u64 LE.
using CapabilityMask = uint64_t;

// The bit for one capability.
[[nodiscard]] inline constexpr CapabilityMask capBit(Capability c) noexcept {
    return CapabilityMask{1} << static_cast<uint8_t>(c);
}

// Every defined bit. A received mask is sanitized against this (`mask & kAllCaps`) so an unknown
// future bit set by a newer client can never be interpreted as a capability we do not implement.
inline constexpr CapabilityMask kAllCaps = (CapabilityMask{1} << static_cast<uint8_t>(Capability::Count)) - 1;

// True when `have` holds every bit in `required`. required == 0 (an unannotated / public command)
// is satisfied by any mask, including zero caps.
[[nodiscard]] inline constexpr bool hasCaps(CapabilityMask have, CapabilityMask required) noexcept {
    return (have & required) == required;
}

// True when a wire-facing byte names a defined capability bit position. Gate an attacker-supplied
// ordinal on this before casting to Capability.
[[nodiscard]] inline constexpr bool isCapabilityOrdinal(uint8_t ordinal) noexcept {
    return ordinal < static_cast<uint8_t>(Capability::Count);
}

// ── Role presets ────────────────────────────────────────────────────────────
// Admin = every bit; this is exactly what a successful operator_password auth grants (rung 1 of the
// grant ladder — byte-for-byte today's all-or-nothing behavior).
inline constexpr CapabilityMask kAdminCaps = kAllCaps;

// Moderator = the kick/ban/mute/spectate tier; no spawn, config, orders, or grant.
inline constexpr CapabilityMask kModeratorCaps =
    capBit(Capability::KickBan) | capBit(Capability::Mute) | capBit(Capability::SpectateAny);

// Game master = the #861 battlespace authority: the map, spawn, AI orders, faction posture, spectate.
inline constexpr CapabilityMask kGameMasterCaps = capBit(Capability::GmMap) | capBit(Capability::SpawnAny) |
                                                  capBit(Capability::CommandAnyAi) |
                                                  capBit(Capability::FactionPosture) | capBit(Capability::SpectateAny);

// Faction leader = the faction-scoped subset (mask only; the faction binding lives in PeerAuthority and
// the scoping enforcement is #948).
[[nodiscard]] inline constexpr CapabilityMask factionLeaderCaps() noexcept {
    return capBit(Capability::SpawnFaction) | capBit(Capability::CommandFactionAi) |
           capBit(Capability::FactionPosture) | capBit(Capability::GmMapFaction);
}

// Human-readable capability name, for permission-refusal messages and the `peers` role column.
[[nodiscard]] inline constexpr std::string_view capabilityName(Capability c) noexcept {
    switch (c) {
    case Capability::KickBan:
        return "kick_ban";
    case Capability::Mute:
        return "mute";
    case Capability::ServerConfig:
        return "server_config";
    case Capability::SpawnAny:
        return "spawn_any";
    case Capability::SpawnFaction:
        return "spawn_faction";
    case Capability::CommandAnyAi:
        return "command_any_ai";
    case Capability::CommandFactionAi:
        return "command_faction_ai";
    case Capability::FactionPosture:
        return "faction_posture";
    case Capability::GmMap:
        return "gm_map";
    case Capability::GmMapFaction:
        return "gm_map_faction";
    case Capability::SpectateAny:
        return "spectate_any";
    case Capability::GrantRoles:
        return "grant_roles";
    case Capability::Count:
        break;
    }
    return "";
}

// The name of the lowest-numbered capability the caller is MISSING from `required` given `have`, for a
// "permission denied: <cmd> requires <cap>" message. Returns "" when nothing is missing.
[[nodiscard]] inline constexpr std::string_view firstMissingCapabilityName(CapabilityMask have,
                                                                           CapabilityMask required) noexcept {
    const CapabilityMask missing = required & ~have;
    for (uint8_t i = 0; i < static_cast<uint8_t>(Capability::Count); ++i) {
        if (missing & (CapabilityMask{1} << i)) {
            return capabilityName(static_cast<Capability>(i));
        }
    }
    return "";
}

// Parse a role-preset name to its capability mask. Used by the grant command. Returns nullopt for an
// unknown preset (the caller surfaces a usage error). The faction-leader mask is returned unbound; the
// grant command carries the faction index separately into PeerAuthority.
[[nodiscard]] inline std::optional<CapabilityMask> parseRolePreset(std::string_view name) noexcept {
    if (name == "admin")
        return kAdminCaps;
    if (name == "moderator" || name == "mod")
        return kModeratorCaps;
    if (name == "gm" || name == "game_master" || name == "gamemaster")
        return kGameMasterCaps;
    if (name == "faction_leader" || name == "faction")
        return factionLeaderCaps();
    return std::nullopt;
}

// Per-peer granted authority. Orthogonal to PeerRole: stored beside role/handshakeComplete on
// PeerInputState, default zero caps (no authority) for every peer. factionIndex binds a faction-scoped
// grant (kNoFactionBinding = unbound / any-faction admin). Ephemeral — erased with the peer on
// disconnect (persistence across reconnect is the identity-bound issue, #950).
struct PeerAuthority {
    static constexpr uint16_t kNoFactionBinding = 0xFFFFu;
    CapabilityMask caps{0};
    uint16_t factionIndex{kNoFactionBinding};

    [[nodiscard]] constexpr bool has(Capability c) const noexcept {
        return (caps & capBit(c)) != 0;
    }
    [[nodiscard]] constexpr bool any() const noexcept {
        return caps != 0;
    }
};

// Who issued a command, threaded through CommandRegistry::dispatch so a command can be
// permission-checked (#946). Named CommandIssuer rather than the #944 "CommandContext" because that
// name is already taken by the game client's console context (engine/console/ConsoleCommands.h, same
// namespace fl). The implicit-Admin paths (stdin console / RCON / single-player --admin-token /
// operator_password auth) use dispatch(line) without an issuer; a default-constructed issuer is Admin
// so constructing one is never a surprise. kIssuerNoPeer mirrors fl::kNoPeer
// (engine/world/Formation.h) without pulling that header, keeping this vocabulary header stdlib-only.
inline constexpr uint32_t kIssuerNoPeer = 0xFFFFFFFFu;

struct CommandIssuer {
    uint32_t peerId{kIssuerNoPeer};                          // kIssuerNoPeer = console / RCON / system
    CapabilityMask caps{kAdminCaps};                         // default: full authority (system issuer)
    uint16_t factionIndex{PeerAuthority::kNoFactionBinding}; // faction binding for a faction-scoped grant (#948)
};

} // namespace fl
