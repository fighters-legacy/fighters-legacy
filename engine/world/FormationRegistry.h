// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/Formation.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// Owns the formation tree and answers the two questions the order path asks:
//   "which formation is this entity in?"  and  "may this peer order that formation?"
//
// Sim-thread only (same contract as WorldBroadcaster's peer maps). No locking.
class FormationRegistry {
  public:
    // Create a formation. `parent` must exist (or be kNoFormation for a top-level formation).
    // Returns kNoFormation if the parent is unknown or the tree would exceed kMaxFormationDepth.
    FormationId create(std::string callsign, EntityId anchor, uint32_t commanderPeerId,
                       FormationId parent = kNoFormation);

    // Remove a formation. Its children are re-parented to ITS parent rather than being destroyed —
    // disbanding a squadron does not shoot down its flights. Members are simply released (their
    // controllers are the caller's business).
    bool destroy(FormationId id);

    // Add an aircraft. An entity may belong to exactly one formation; adding it to a second moves it.
    // Returns false if the formation is unknown or the entity is invalid.
    bool addMember(FormationId id, FormationMember member);

    // Remove an entity from whatever formation holds it (a kill, or a peer disconnecting).
    // Returns the formation it was removed from, or kNoFormation.
    FormationId removeEntity(EntityId entity);

    [[nodiscard]] const Formation* get(FormationId id) const noexcept;
    [[nodiscard]] Formation* get(FormationId id) noexcept;

    // The formation holding `entity` as a MEMBER (not as an anchor), or kNoFormation.
    [[nodiscard]] FormationId formationOfEntity(EntityId entity) const noexcept;

    // The formation anchored on `entity` (i.e. the one this player leads), or kNoFormation.
    [[nodiscard]] FormationId formationAnchoredOn(EntityId entity) const noexcept;

    // AUTHORITY. True when `peerId` commands `id` directly, or commands any ANCESTOR of it — command
    // cascades down the tree, which is what makes a package commander able to order a flight inside
    // it without being that flight's own commander. kNoPeer (the game master) is NOT granted
    // authority here: the admin console bypasses this check deliberately and visibly, rather than by
    // forging a peer identity.
    [[nodiscard]] bool commands(uint32_t peerId, FormationId id) const noexcept;

    // `id` plus every descendant, parents before children. The unit an order cascades over.
    [[nodiscard]] std::vector<FormationId> subtree(FormationId id) const;

    // Formations this peer commands DIRECTLY (not via an ancestor). Used to answer "what do I lead?"
    [[nodiscard]] std::vector<FormationId> commandedBy(uint32_t peerId) const;

    // Drop a disconnecting peer: clears its command role everywhere (the formation survives, but
    // becomes game-master-only) and removes any aircraft it was flying as a member.
    void releasePeer(uint32_t peerId);

    void forEach(const std::function<void(const Formation&)>& fn) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return m_formations.size();
    }
    void clear() noexcept;

  private:
    [[nodiscard]] uint8_t depthOf(FormationId id) const noexcept;

    std::unordered_map<FormationId, Formation> m_formations;
    // entity pool index -> formation holding it as a member. The reverse index that makes
    // formationOfEntity O(1) on the order path.
    std::unordered_map<uint32_t, FormationId> m_memberIndex;
    FormationId m_nextId{1}; // 0 is kNoFormation
};

} // namespace fl
