// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Quantized per-entity snapshot record codec — the single audited encode/decode path shared by the
// server (WorldBroadcaster) and the client (ClientNetEventHandler), analogous to WireCodec.h for the
// fixed byte structs. Records are bit-packed (BitStream.h) and quantized (Quantization.h) into the
// body of MsgWorldSnapshot, after the origin table and before the TLV block.
//
// Encode-once (#725): each entity is quantized relative to a SHARED per-region origin — the floor of
// its position onto a fixed kOriginGridM grid — instead of the receiving peer's position. That makes
// a record peer-INDEPENDENT, so the sim encodes each entity ONCE per tick and each peer's snapshot is
// assembled by stitching pre-encoded record blobs (memcpy), not re-quantizing per peer. Two things
// make a record peer-independent: (1) position is relative to the shared grid origin, and (2) the
// entity index is written ABSOLUTELY (not as a delta against the previous record) and each record is
// BYTE-ALIGNED, so a blob can be dropped into any peer's stream in any order. Per peer, the snapshot
// carries an ORIGIN TABLE (the distinct grid origins its records reference, deduped) and each record
// is prefixed with an origin index into that table (written at stitch time, not baked into the blob).
//
// Static/unused fields are omitted from a record:
//   * typeIndex   — only in `full` records (client caches it per entity);
//   * gen         — only when it changed (`genPresent`); else the client reuses its cache;
//   * omega       — only on the receiving peer's OWN entity (`hasOmega`); the sole client consumer
//                   is client-side prediction reconciliation.
//
// Wire layout of one STITCHED record (byte-aligned; MSB-first within the blob):
//   originIndex : varint (index into the snapshot's origin table; written by the stitch)
//   -- blob (encodeStandaloneRecord), byte-aligned: --
//   idx      : varint (absolute entity index)
//   full     : 1 bit
//   genPresent : 1 bit
//   omegaPresent : 1 bit
//   gen      : 16 bits        (only if genPresent)
//   typeIndex: varint         (only if full)
//   pos[3]   : kPosBitsPerAxis each, signed offset from the grid origin at kPosStepM resolution
//   ori      : 2-bit dropped-component index + 3 x kQuatBits (smallest-three)
//   vel[3]   : kVelBits each, range +/- kVelMaxMps
//   omega[3] : kOmegaBits each, range +/- kOmegaMaxRadS   (only if omegaPresent)
//   damageLevel : kDamageBits | engineFailFlags : kEngineFailBits | throttle : kThrottleBits |
//   fuelPct : kFuelBits | abEngaged : 1 | playerOwned : 1
//   (zero-padded to the next byte boundary)

#include <cstdint>
#include <vector>

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
    uint16_t factionIndex{0};          // FactionRegistry index; carried on full records, client-cached (#860)
    bool isFull{false};                // full record: carries typeIndex, factionIndex (and gen)
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

    // ── own-record extras (#625) — gated by the SAME hasOmega bit ───────────
    // The receiving peer's own record carries its live loadout: selection, rounds on the selected
    // station, seeker flags, and the payload the remaining stores cost the airframe RIGHT NOW.
    // This is the GameProtocol.h:125 seam executed: once a store can leave the rails, the per-type
    // static payload on MsgEntityTypeDef stops being the truth (it remains the pre-first-snapshot
    // fallback), and ClientPrediction re-resolves from here so a released store changes the
    // client-predicted physics too. No new flag bit: omega and the loadout are both own-record-only.
    uint8_t selectedStation{255};
    uint16_t stationRounds{0};
    uint8_t weaponFlags{0};   // bit 0 = seeker locked (#628)
    float payloadMassKg{0.f}; // quantized 1 kg steps, [0, 65535]
    float payloadCd0{0.f};    // quantized 1e-5 steps, [0, 0.65535]
};

// Shared quantization origin grid (#725). Each entity is quantized relative to the origin of the
// grid cell containing it: origin[i] = floor(pos[i] / kOriginGridM) * kOriginGridM. ~65 km keeps the
// per-axis offset well inside the fixed-point position range (kPosBitsPerAxis) at kPosStepM
// resolution, and is coarse enough that spatially-clustered visible entities share a handful of
// origins (a small per-peer origin table).
inline constexpr double kOriginGridM = 65536.0;

// Grid-cell origin (the shared quantization origin) for a world position.
void originForPos(const double pos[3], double out[3]) noexcept;

// Encode one entity into a self-contained, BYTE-ALIGNED record blob with an ABSOLUTE index varint,
// position relative to `origin` (its grid-cell origin). Appends the blob's bytes to `out`. The blob
// is peer-INDEPENDENT — the per-peer origin index is written separately by appendStitchedRecord — so
// this is the once-per-tick encode whose output is stitched into every peer's stream. `sendGen`
// controls the genPresent bit (caller policy: true for full or when gen changed since the peer last
// saw the entity).
void encodeStandaloneRecord(std::vector<uint8_t>& out, const QuantEntity& e, const double origin[3], bool sendGen);

// Stitch one pre-encoded record into a peer's byte-aligned record stream: appends the origin index
// (varint, into that snapshot's origin table) followed by the record `blob`. Keeps the stream
// byte-aligned so the next record's origin index starts on a byte boundary.
void appendStitchedRecord(std::vector<uint8_t>& stream, uint32_t originIndex, const std::vector<uint8_t>& blob);

// Decode one stitched record from a reader positioned at a byte-aligned record boundary: reads the
// origin-index varint, resolves the origin from `originTable` (originCount entries of double[3]),
// decodes the body, and leaves the reader byte-aligned at the next record. genPresent reports whether
// gen was on the wire (else out.gen is left for the caller to fill from cache); typeIndex is only set
// when out.isFull. Returns false on truncation or an origin index >= originCount.
[[nodiscard]] bool decodeStandaloneRecord(BitReader& r, QuantEntity& out, const double* originTable,
                                          uint32_t originCount, bool& genPresent);

// Estimated stitched size in bytes of one record with the given shape — the byte cost the priority/
// budget scheduler (#516) accounts per candidate. Mirrors the stitched layout exactly; the value-
// dependent parts are the origin-index, absolute-idx, and typeIndex varints plus the per-record byte
// alignment. Returns a per-record upper bound (each record is byte-aligned), so summing it over
// admitted records never under-counts the encoded size.
[[nodiscard]] uint32_t estimateRecordBytes(bool isFull, bool sendGen, bool hasOmega, uint32_t typeIndex,
                                           uint32_t entityIndex, uint32_t originIndex) noexcept;

} // namespace fl
