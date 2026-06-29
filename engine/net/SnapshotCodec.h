// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Quantized per-entity snapshot record codec — the single audited encode/decode path shared by the
// server (WorldBroadcaster) and the client (ClientNetEventHandler), analogous to WireCodec.h for the
// fixed byte structs. Records are bit-packed (BitStream.h) and quantized (Quantization.h) into the
// body of MsgWorldSnapshot, after the 40-byte MsgWorldSnapshotHeader and before the TLV block.
//
// Position is encoded RELATIVE to the per-snapshot double frame origin carried in the header, so the
// stream stays planet-scale accurate without a double per record. Static/unused fields are omitted:
//   * typeIndex   — only in `full` records (client caches it per entity);
//   * gen         — only when it changed (`genPresent`); else the client reuses its cache;
//   * omega       — only on the receiving peer's OWN entity (`hasOmega`); the sole client consumer
//                   is client-side prediction reconciliation.
//
// Wire bit-layout of one record (MSB-first):
//   idxDelta : varint (cur.idx - prevIdx; records are sorted by idx ascending, so delta >= 0)
//   full     : 1 bit
//   genPresent : 1 bit
//   omegaPresent : 1 bit
//   gen      : 16 bits        (only if genPresent)
//   typeIndex: varint         (only if full)
//   pos[3]   : kPosBitsPerAxis each, signed offset from frame origin at kPosStepM resolution
//   ori      : 2-bit dropped-component index + 3 x kQuatBits (smallest-three)
//   vel[3]   : kVelBits each, range +/- kVelMaxMps
//   omega[3] : kOmegaBits each, range +/- kOmegaMaxRadS   (only if omegaPresent)
//   damageLevel : kDamageBits | engineFailFlags : kEngineFailBits | throttle : kThrottleBits |
//   fuelPct : kFuelBits | abEngaged : 1 | playerOwned : 1

#include <cstdint>

namespace fl {

class BitWriter;
class BitReader;

// --- Quantization budget (tuned against the bot_swarm downstream_kbs_per_client gate of 150 KB/s) ---
inline constexpr double kPosStepM = 0.125;    // 12.5 cm position resolution
inline constexpr int kPosBitsPerAxis = 22;    // +/- 2^21 * step = +/- 262 km from frame origin
inline constexpr int kQuatBits = 10;          // per smallest-three component
inline constexpr int kVelBits = 18;           // per velocity axis (~0.015 m/s resolution)
inline constexpr double kVelMaxMps = 2000.0;  // velocity clamp range (m/s)
inline constexpr int kOmegaBits = 12;         // per angular-rate axis
inline constexpr double kOmegaMaxRadS = 20.0; // angular-rate clamp range (rad/s)
inline constexpr int kDamageBits = 3;         // damageLevel 0..7
inline constexpr int kEngineFailBits = 5;     // kEngineFail* bitmask (up to 0x10)
inline constexpr int kThrottleBits = 7;       // 0..100
inline constexpr int kFuelBits = 7;           // 0..100

// Plain-POD transfer struct (no glm): the decoded/about-to-encode state of one entity. Position is
// absolute world coordinates; the codec converts to/from the frame origin internally.
struct QuantEntity {
    uint32_t idx{0};
    uint32_t gen{0};
    uint32_t typeIndex{0};
    bool isFull{false};                // full record: carries typeIndex (and gen)
    bool hasOmega{false};              // carries omega (set only for the receiving peer's own entity)
    double pos[3]{};                   // absolute world position (m)
    float vel[3]{};                    // world-frame velocity (m/s)
    float quat[4]{0.f, 0.f, 0.f, 1.f}; // orientation x,y,z,w
    float omega[3]{};                  // body-frame angular rates p,q,r (rad/s)
    uint8_t damageLevel{0};
    uint8_t engineFailFlags{0};
    uint8_t throttle{0};
    uint8_t fuelPct{0};
    bool abEngaged{false};
    bool playerOwned{false};
};

// Encode one record. prevIdx is updated to e.idx (pass 0 before the first record of a snapshot).
// sendGen controls the genPresent bit (caller policy: true for full or when gen changed since the
// peer last saw the entity).
void encodeRecord(BitWriter& w, const QuantEntity& e, uint32_t& prevIdx, const double origin[3], bool sendGen);

// Decode one record. prevIdx is updated to the decoded idx. genPresent reports whether gen was on
// the wire (else out.gen is left untouched for the caller to fill from cache); typeIndex is only set
// when out.isFull. Returns false on a truncated/malformed buffer.
[[nodiscard]] bool decodeRecord(BitReader& r, QuantEntity& out, uint32_t& prevIdx, const double origin[3],
                                bool& genPresent);

} // namespace fl
