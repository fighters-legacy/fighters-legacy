// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// Flight-deck geometry and carrier-ops parameters for an entity that accepts landings (#38) — the
// consumer of the #699 `acceptsLandings` seam. All coordinates are SHIP-LOCAL: x forward along the
// keel, y up, z starboard, origin at the entity's transform (the waterline position the sim
// integrates). The deck itself is a plane at heightM above that origin, lengthM x widthM around it.
//
// The deck's physics is the existing radial ground floor: the server composes "the floor under
// this aircraft" as max(terrain, deck plane) whenever the aircraft is over the footprint and at or
// above deck level — so landing, parking, and the takeoff roll on a MOVING ship reuse the exact
// ground-handling path a runway uses, plus a deck-carry term that moves parked aircraft with the
// ship. The footprint travels on the wire (MsgEntityTypeDef tail) so client prediction composes
// the same moving floor; the catapult and arrest parameters stay server-side (server-authoritative
// events).
struct DeckDef {
    float lengthM{330.f}; // footprint along the keel
    float widthM{75.f};   // footprint abeam
    float heightM{20.f};  // deck plane above the ship origin (waterline)

    // Catapult: a stroke starting at catStartXM on the centreline, running FORWARD (+x) for
    // catStrokeM, releasing at catEndSpeedMps. An aircraft stopped on the stroke at full military
    // power is hooked up and shot.
    float catStartXM{30.f};
    float catStrokeM{100.f};
    float catEndSpeedMps{75.f};

    // Arrest: a wire zone centred at wireXM spanning wireZoneM along the keel. A touchdown inside
    // it at or below maxTrapSpeedMps catches a wire and is dragged to a stop; faster is a bolter.
    float wireXM{-110.f};
    float wireZoneM{40.f};
    float maxTrapSpeedMps{80.f};
};

// A world position expressed in the deck's ship-local frame (x fwd, y up, z starboard), plus the
// horizontal-footprint test. Pure scalar math (no glm) so engine-entity stays dependency-free and
// the server pass, client prediction, and unit tests all run the identical transform.
struct DeckLocalPoint {
    float x{0.f}, y{0.f}, z{0.f};
    bool inFootprint{false}; // |x| <= length/2 and |z| <= width/2
};

[[nodiscard]] inline DeckLocalPoint deckLocalPoint(const double worldPos[3], const double shipPos[3],
                                                   const float shipQuat[4], const DeckDef& deck) noexcept {
    // Rotate (worldPos - shipPos) by the CONJUGATE of the ship quaternion (world -> body).
    const double dx = worldPos[0] - shipPos[0];
    const double dy = worldPos[1] - shipPos[1];
    const double dz = worldPos[2] - shipPos[2];
    const float qx = -shipQuat[0], qy = -shipQuat[1], qz = -shipQuat[2], qw = shipQuat[3];
    const double tx = 2.0 * (double(qy) * dz - double(qz) * dy);
    const double ty = 2.0 * (double(qz) * dx - double(qx) * dz);
    const double tz = 2.0 * (double(qx) * dy - double(qy) * dx);
    DeckLocalPoint p;
    p.x = static_cast<float>(dx + qw * tx + double(qy) * tz - double(qz) * ty);
    p.y = static_cast<float>(dy + qw * ty + double(qz) * tx - double(qx) * tz);
    p.z = static_cast<float>(dz + qw * tz + double(qx) * ty - double(qy) * tx);
    p.inFootprint = (p.x >= -deck.lengthM * 0.5f && p.x <= deck.lengthM * 0.5f && p.z >= -deck.widthM * 0.5f &&
                     p.z <= deck.widthM * 0.5f);
    return p;
}

// Whether the deck floor applies to an aircraft at `local`: over the footprint and NOT clearly
// below deck level (a low pass under the bow must not be teleported up onto the deck). The small
// tolerance admits an aircraft settling into ground contact from just below the plane.
[[nodiscard]] inline bool deckFloorApplies(const DeckLocalPoint& local, const DeckDef& deck) noexcept {
    return local.inFootprint && local.y >= deck.heightM - 2.f;
}

} // namespace fl
