// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// ── Client-side crew state (#966/#972) ───────────────────────────────────────
//
// The client's view of a crewed aircraft: the reliable seat ROSTER (from MsgCrewRoster) and the
// per-tick live turret POSE (from the SnapshotCrew TLV). Kept as plain data in the game layer so the
// seat-selection UI (#975) and the gunner station (#979) consume it without reaching into the network
// handler. A single-seat aircraft has no roster (the server never sends one), so absence == "not a
// crewed aircraft" from the client's point of view.

// One seat as the client knows it (mirror of CrewRosterSeat, with role decoded to a std::string).
struct CrewSeatInfo {
    uint8_t seatIndex{0};
    uint8_t occupancy{0};                 // fl::SeatOccupancy ordinal (Empty/Bot/Human)
    uint16_t capabilities{0};             // fl::CrewCapabilityMask
    uint32_t occupantPeerId{0xFFFFFFFFu}; // human peer id when occupancy == Human, else the kNoSeatPeer sentinel
    uint8_t skillPct{50};
    uint8_t turretIndex{255}; // turret this seat aims (index into turretPoses); 255 = none
    std::string role;
};

// One crewed aircraft's roster, keyed by (entityIdx, entityGen).
struct CrewRosterInfo {
    uint32_t entityIdx{0};
    uint32_t entityGen{0};
    uint8_t turretCount{0};
    std::vector<CrewSeatInfo> seats;
};

// Live mount-frame pose of one turret (from the SnapshotCrew TLV), decoded from the quantized wire az/el.
struct CrewTurretPose {
    float azRad{0.f};
    float elRad{0.f};
};

} // namespace fl
