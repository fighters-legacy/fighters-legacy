// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"

#include <cstdint>
#include <string>
#include <vector>

// The formation / command hierarchy (#610).
//
// Real air operations are TIERED, and the tiers are the whole point: an ELEMENT (a lead and a
// wingman) sits inside a FLIGHT (typically two elements), flights sit inside a PACKAGE or squadron
// under a mission commander, and a controlling agency (AWACS/GCI) directs from outside the chain
// entirely. Orders flow DOWN that tree. Modelling only "a player and their wingman" would have been
// a special case of it — one node, one member — and would have had to be torn out the moment a
// second element, an AI-only flight, or a strike package appeared.
//
// So the unit of organisation is a FORMATION NODE, and everything else is a shape of the tree:
//
//   * A single player with one AI wingman  -> one node: anchor = the player, one AI member.
//   * Two human pilots flying as a pair    -> one node: anchor = the lead player, one HUMAN member.
//   * An all-AI flight run by an AWACS     -> one node whose commander is a peer that is NOT in it.
//   * A strike package                     -> a parent node with child flights; the package commander
//                                             commands the children transitively (see `commands()`).
//
// Two properties fall out of this and are load-bearing:
//
//  1. THE COMMANDER IS A ROLE, NOT A SEAT. `commanderPeerId` is whoever may give this formation
//     orders; they need not be its anchor, need not be a member, and need not be in the tree at all
//     (an AWACS player, or the game master via the `flight` admin command). Authority is checked as
//     "do you command this node, or an ancestor of it" — never as "do you own this entity".
//
//  2. A MEMBER IS AN AIRCRAFT, NOT AN AI. Members may be AI (peerId == 0, server-controlled: the
//     server retasks the controller) or HUMAN (peerId != 0: the server CANNOT retask a person, so the
//     order is relayed to them as a radio call and compliance is their choice). That distinction is
//     the honest one, and it is why an order returns Acknowledged for one and Relayed for the other.
//
// Sim-thread only, like every other roster on WorldBroadcaster. Pure stdlib (EntityId.h is a
// header-only POD), so engine-world stays dependency-free.

namespace fl {

using FormationId = uint16_t;

// 0 is never a valid formation. Reserved as "none / not in a formation".
inline constexpr FormationId kNoFormation = 0;

// Depth cap on the command tree. Element -> flight -> squadron -> package -> mission is five; the cap
// is a cycle/runaway guard, not a doctrinal statement.
inline constexpr uint8_t kMaxFormationDepth = 8;

struct FormationMember {
    EntityId id;
    // Peer flying this aircraft; 0 = AI (server-controlled). This single field is what separates
    // "retask the controller" from "relay a radio call and hope".
    uint32_t peerId{0};
    // Station in the formation geometry (ai::formationSlotOffset). Slot 0 is the first member.
    uint32_t slotIndex{0};
    // Set by hold_fire, cleared by an engage order. Has no teeth until weapons land (#583) — it is
    // stored here now so the order is not silently dropped in the meantime.
    bool weaponsHold{false};

    [[nodiscard]] bool isAi() const noexcept {
        return peerId == 0;
    }
};

struct Formation {
    FormationId id{kNoFormation};
    FormationId parent{kNoFormation}; // kNoFormation = top of its chain
    std::string callsign;             // "Viper", "Uzi 3" — display + admin addressing
    EntityId anchor;                  // the entity members fly formation ON: a player, or an AI lead
    uint32_t commanderPeerId{0};      // who may order this formation; 0 = server/game-master only
    std::vector<FormationMember> members;
    std::vector<FormationId> children;
};

} // namespace fl
