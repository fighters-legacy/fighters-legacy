// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>

// The client-facing view of the datalink track picture + RWR (#528), consumed by the HUD. Lives in
// engine/render (not engine/sensor) so the HUD does not pull the whole sensor library across the
// layer boundary: these are plain POD with absolute world positions the client reconstructed from the
// MsgDatalink relative payload. The ordinals mirror the engine enums by VALUE (documented per field)
// so no header dependency is needed — the HUD only ever compares them against the named constants
// below.

namespace fl {

// Mirrors sensor::Identification ordinals (Iff.h). The display-safe fact — what the observer has
// IDENTIFIED, never the target's raw faction.
inline constexpr uint8_t kIffUnknown = 0;
inline constexpr uint8_t kIffFriend = 1;
inline constexpr uint8_t kIffFoe = 2;

// Mirrors sensor::ContactState ordinals (Detection.h).
inline constexpr uint8_t kTrackLost = 0;
inline constexpr uint8_t kTrackDetected = 1;
inline constexpr uint8_t kTrackLocked = 2;
inline constexpr uint8_t kTrackCoasting = 3;

// Mirrors sensor::ThreatLevel ordinals (SensorSystem.h): a scan strobe, a lock tone, and a launch
// (a radar-guided missile guiding on you — #960).
inline constexpr uint8_t kThreatSearch = 0;
inline constexpr uint8_t kThreatLock = 1;
inline constexpr uint8_t kThreatLaunch = 2;

// One fused track for display. Position is ABSOLUTE world metres (double), reconstructed on the client
// from the datalink header origin + the record's relative offset.
struct RadarTrack {
    double pos[3]{};
    float vel[3]{};
    uint32_t entityIdx{0};
    uint16_t entityGen{0};
    uint8_t state{0};          // kTrack*
    uint8_t ident{0};          // kIff*
    uint8_t sensorTypeMask{0}; // 1<<SensorType; which kinds of sensor hold it across the team
    bool firingQuality{false}; // a firing-quality (STT) lock is on it
    bool ownSensor{false};     // this peer's own sensors hold it (else datalink-only)
};

// One RWR strobe for display. `emitterPos` is ABSOLUTE world metres.
struct RwrStrobe {
    double emitterPos[3]{};
    uint32_t emitterIdx{0};
    uint8_t channel{0}; // SensorType ordinal (radar/laser)
    uint8_t level{0};   // kThreat*
    uint8_t ident{0};   // kIff* of the emitter — a friendly emitter is benign
};

// Everything the HUD needs to draw the radar scope + RWR this frame. `valid` is false until the first
// datalink message arrives, so the HUD draws nothing rather than an empty scope on connect.
struct RadarView {
    std::span<const RadarTrack> tracks;
    std::span<const RwrStrobe> strobes;
    bool valid{false};
};

} // namespace fl
