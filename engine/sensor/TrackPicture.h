// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "sensor/Detection.h"    // ContactState
#include "sensor/Iff.h"          // Identification
#include "sensor/SensorSystem.h" // Contact, ContactTable

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fl::sensor {

// One entry in a fused team track picture (#528): a target, merged across every observer that
// contributed a contact on it. This is the shared-awareness datum a datalink delivers — you see the
// bandit your wingman locked even if your own radar never found it.
struct FusedTrack {
    EntityId id{};
    uint32_t typeIndex{0};
    uint16_t factionIndex{0};
    ContactState state{ContactState::Lost};
    Identification ident{Identification::Unknown};
    uint8_t sensorTypeMask{0}; // OR of every contributing observer's mask
    bool firingQuality{false}; // any contributor holds a firing-quality lock
    bool ownSensor{false};     // the REQUESTING peer's own sensors hold it (not only the datalink)
    double lastKnownPos[3]{};  // from the freshest contributing contact
    float lastKnownVel[3]{};   //
    uint64_t lastSeenTick{0};  // freshest contributor
};

// Fuses many observers' contact tables into one deduplicated picture keyed by target. Deterministic:
// the merge is commutative/associative (max-rank, freshest-position, OR-of-flags, best-identification)
// so the fused set does not depend on the order tables are added — the same property the sensing pass
// guarantees, extended to fusion. `tracks()` is returned sorted by target index for a stable wire.
class TrackFuser {
  public:
    void reset() {
        m_byTarget.clear();
    }

    // Merge one observer's table. `ownSensor` marks it as the requesting peer's OWN table, so the
    // fused entry can distinguish "I see this myself" from "only the datalink shows me this".
    //
    // Since #1088 a team's picture is fused ONCE per faction with ownSensor=false throughout, and the
    // per-peer "I hold this myself" bit is overlaid afterwards from the peer's own contact table —
    // fusing is a property of the faction, marking is a property of the peer.
    void add(const ContactTable& table, bool ownSensor) {
        for (const Contact& c : table) {
            FusedTrack& t = m_byTarget[c.id.index];
            const bool first = (t.id.generation == 0);

            // Pool-slot reuse guard: a stale target index may map to a different generation, meaning
            // two observers hold contacts on two DIFFERENT entities that happen to share a pool slot.
            // The NEWER generation wins, and an older one is dropped outright.
            //
            // This used to be "whichever was merged last resets the entry", which made the fused
            // result depend on the order tables were added — quietly contradicting this class's own
            // order-independence contract, and only invisible because the per-peer caller always
            // merged in the same order. Per-faction fusion (#1088) merges once for a whole team, so
            // the tie has to be decided by the data rather than by arrival order.
            if (!first && t.id.generation != c.id.generation) {
                if (c.id.generation < t.id.generation)
                    continue; // stale slot: a newer entity already owns this index
                t = FusedTrack{};
            }

            t.id = c.id;
            t.typeIndex = c.typeIndex;
            t.factionIndex = c.factionIndex;
            t.sensorTypeMask |= c.sensorTypeMask;
            t.firingQuality = t.firingQuality || c.firingQuality;
            t.ownSensor = t.ownSensor || ownSensor;

            // Best identification wins, order-independent: a Foe anywhere makes it a Foe, else a Friend
            // makes it a Friend, else Unknown. (Teammates share a faction, so conflicting IDs of the
            // same target do not arise in practice; this rule is simply well-defined if they ever do.)
            if (c.ident == Identification::Foe)
                t.ident = Identification::Foe;
            else if (c.ident == Identification::Friend && t.ident != Identification::Foe)
                t.ident = Identification::Friend;

            // Freshest contributor supplies the state and last-known kinematics (a coasting teammate's
            // stale fix loses to a live one). Tiebreak on state rank so a lock outranks a mere detect.
            const bool fresher = c.lastSeenTick > t.lastSeenTick ||
                                 (c.lastSeenTick == t.lastSeenTick && stateRank(c.state) >= stateRank(t.state));
            if (first || fresher) {
                t.state = c.state;
                t.lastSeenTick = c.lastSeenTick;
                t.lastKnownPos[0] = c.lastKnownPos[0];
                t.lastKnownPos[1] = c.lastKnownPos[1];
                t.lastKnownPos[2] = c.lastKnownPos[2];
                t.lastKnownVel[0] = c.lastKnownVel[0];
                t.lastKnownVel[1] = c.lastKnownVel[1];
                t.lastKnownVel[2] = c.lastKnownVel[2];
            }
        }
    }

    // The fused picture, sorted by target index (stable wire order).
    [[nodiscard]] std::vector<FusedTrack> tracks() const {
        std::vector<FusedTrack> out;
        out.reserve(m_byTarget.size());
        for (const auto& [idx, t] : m_byTarget)
            out.push_back(t);
        std::sort(out.begin(), out.end(),
                  [](const FusedTrack& a, const FusedTrack& b) { return a.id.index < b.id.index; });
        return out;
    }

  private:
    // Match the contact-cap ranking so "freshest, then best state" is consistent with the sensing pass.
    [[nodiscard]] static int stateRank(ContactState s) noexcept {
        switch (s) {
        case ContactState::Locked:
            return 3;
        case ContactState::Detected:
            return 2;
        case ContactState::Coasting:
            return 1;
        case ContactState::Lost:
            return 0;
        }
        return 0;
    }

    std::unordered_map<uint32_t, FusedTrack> m_byTarget;
};

} // namespace fl::sensor
