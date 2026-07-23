// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/Capability.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

using fl::Capability;
using fl::CapabilityMask;
using fl::capabilityName;
using fl::capBit;
using fl::factionLeaderCaps;
using fl::firstMissingCapabilityName;
using fl::hasCaps;
using fl::isCapabilityOrdinal;
using fl::kAdminCaps;
using fl::kAllCaps;
using fl::kGameMasterCaps;
using fl::kModeratorCaps;
using fl::parseRolePreset;
using fl::PeerAuthority;

TEST_CASE("capability bits are unique and within the mask width", "[capability]") {
    CapabilityMask seen = 0;
    for (uint8_t i = 0; i < static_cast<uint8_t>(Capability::Count); ++i) {
        const CapabilityMask bit = capBit(static_cast<Capability>(i));
        CHECK(bit != 0);
        CHECK((seen & bit) == 0); // no two capabilities share a bit
        seen |= bit;
    }
    // kAllCaps is exactly the union of every defined bit.
    CHECK(seen == kAllCaps);
    // 12 capabilities fit comfortably in a uint64 mask.
    CHECK(static_cast<uint8_t>(Capability::Count) == 12);
}

TEST_CASE("hasCaps requires every bit in the required mask", "[capability]") {
    const CapabilityMask have = capBit(Capability::KickBan) | capBit(Capability::Mute);
    CHECK(hasCaps(have, capBit(Capability::KickBan)));
    CHECK(hasCaps(have, capBit(Capability::KickBan) | capBit(Capability::Mute)));
    CHECK_FALSE(hasCaps(have, capBit(Capability::SpawnAny)));
    CHECK_FALSE(hasCaps(have, capBit(Capability::KickBan) | capBit(Capability::SpawnAny)));
    // required == 0 (a public command) is satisfied by any mask, including zero caps.
    CHECK(hasCaps(0u, 0u));
    CHECK(hasCaps(have, 0u));
}

TEST_CASE("role presets compose the expected bits", "[capability]") {
    // Admin is every bit.
    CHECK(kAdminCaps == kAllCaps);
    CHECK(hasCaps(kAdminCaps, kAllCaps));

    // Moderator: kick/ban, mute, spectate; nothing else.
    CHECK(hasCaps(kModeratorCaps, capBit(Capability::KickBan)));
    CHECK(hasCaps(kModeratorCaps, capBit(Capability::Mute)));
    CHECK(hasCaps(kModeratorCaps, capBit(Capability::SpectateAny)));
    CHECK_FALSE(hasCaps(kModeratorCaps, capBit(Capability::SpawnAny)));
    CHECK_FALSE(hasCaps(kModeratorCaps, capBit(Capability::GrantRoles)));
    // A moderator cannot self-elevate: it lacks GrantRoles.
    CHECK_FALSE(hasCaps(kModeratorCaps, capBit(Capability::GrantRoles)));

    // Game master: the map, spawn, AI orders, posture, spectate.
    CHECK(hasCaps(kGameMasterCaps, capBit(Capability::GmMap)));
    CHECK(hasCaps(kGameMasterCaps, capBit(Capability::SpawnAny)));
    CHECK(hasCaps(kGameMasterCaps, capBit(Capability::CommandAnyAi)));
    CHECK(hasCaps(kGameMasterCaps, capBit(Capability::FactionPosture)));
    CHECK(hasCaps(kGameMasterCaps, capBit(Capability::SpectateAny)));
    CHECK_FALSE(hasCaps(kGameMasterCaps, capBit(Capability::GrantRoles)));
    CHECK_FALSE(hasCaps(kGameMasterCaps, capBit(Capability::ServerConfig)));

    // Faction leader: the faction-scoped subset.
    const CapabilityMask fl = factionLeaderCaps();
    CHECK(hasCaps(fl, capBit(Capability::SpawnFaction)));
    CHECK(hasCaps(fl, capBit(Capability::CommandFactionAi)));
    CHECK(hasCaps(fl, capBit(Capability::FactionPosture)));
    CHECK(hasCaps(fl, capBit(Capability::GmMapFaction)));
    CHECK_FALSE(hasCaps(fl, capBit(Capability::SpawnAny)));
    CHECK_FALSE(hasCaps(fl, capBit(Capability::GmMap)));
}

TEST_CASE("parseRolePreset round-trips the known presets", "[capability]") {
    CHECK(parseRolePreset("admin") == kAdminCaps);
    CHECK(parseRolePreset("moderator") == kModeratorCaps);
    CHECK(parseRolePreset("mod") == kModeratorCaps);
    CHECK(parseRolePreset("gm") == kGameMasterCaps);
    CHECK(parseRolePreset("game_master") == kGameMasterCaps);
    CHECK(parseRolePreset("faction_leader") == factionLeaderCaps());
    CHECK(parseRolePreset("faction") == factionLeaderCaps());
    // Unknown presets decline.
    CHECK_FALSE(parseRolePreset("superuser").has_value());
    CHECK_FALSE(parseRolePreset("").has_value());
}

TEST_CASE("capability ordinal validation gates wire-facing bytes", "[capability]") {
    CHECK(isCapabilityOrdinal(0));
    CHECK(isCapabilityOrdinal(static_cast<uint8_t>(Capability::GrantRoles)));
    CHECK_FALSE(isCapabilityOrdinal(static_cast<uint8_t>(Capability::Count)));
    CHECK_FALSE(isCapabilityOrdinal(200));
}

TEST_CASE("capability names cover every bit and are non-empty", "[capability]") {
    for (uint8_t i = 0; i < static_cast<uint8_t>(Capability::Count); ++i) {
        CHECK_FALSE(capabilityName(static_cast<Capability>(i)).empty());
    }
    CHECK(capabilityName(Capability::GmMap) == std::string_view{"gm_map"});
    CHECK(capabilityName(Capability::KickBan) == std::string_view{"kick_ban"});
}

TEST_CASE("firstMissingCapabilityName reports the lowest missing bit", "[capability]") {
    // Have KickBan, need KickBan + SpawnAny -> SpawnAny is missing.
    CHECK(firstMissingCapabilityName(capBit(Capability::KickBan),
                                     capBit(Capability::KickBan) | capBit(Capability::SpawnAny)) ==
          std::string_view{"spawn_any"});
    // Nothing missing -> empty.
    CHECK(firstMissingCapabilityName(kAdminCaps, capBit(Capability::GmMap)).empty());
    // Lowest-numbered missing bit wins when several are absent.
    CHECK(firstMissingCapabilityName(0u, capBit(Capability::SpawnAny) | capBit(Capability::KickBan)) ==
          std::string_view{"kick_ban"});
}

TEST_CASE("PeerAuthority defaults to zero caps and no faction binding", "[capability]") {
    PeerAuthority a{};
    CHECK(a.caps == 0);
    CHECK(a.factionIndex == PeerAuthority::kNoFactionBinding);
    CHECK_FALSE(a.any());
    CHECK_FALSE(a.has(Capability::GmMap));

    a.caps = kGameMasterCaps;
    a.factionIndex = 3;
    CHECK(a.any());
    CHECK(a.has(Capability::GmMap));
    CHECK_FALSE(a.has(Capability::GrantRoles));
    CHECK(a.factionIndex == 3);
}
