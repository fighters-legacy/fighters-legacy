// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/SnapshotCodec.h"

#include "net/BitStream.h"
#include "net/Quantization.h"

#include <algorithm>
#include <cmath>

namespace fl {

void originForPos(const double pos[3], double out[3]) noexcept {
    for (int i = 0; i < 3; ++i)
        out[i] = std::floor(pos[i] / kOriginGridM) * kOriginGridM;
}

// Writes the peer-independent record body (absolute idx + fields) into `w`. Shared by encode; the
// caller byte-aligns and the stitch prepends the origin index.
static void encodeBody(BitWriter& w, const QuantEntity& e, const double origin[3], bool sendGen) {
    w.writeVarint(e.idx); // absolute entity index (peer-independent; not a per-stream delta)

    w.writeBits(e.isFull ? 1u : 0u, 1);
    w.writeBits(sendGen ? 1u : 0u, 1);
    w.writeBits(e.hasOmega ? 1u : 0u, 1);

    if (sendGen)
        w.writeBits(e.gen & 0xFFFFu, 16);
    if (e.isFull)
        w.writeVarint(e.typeIndex);

    // Position relative to the shared grid origin.
    for (int i = 0; i < 3; ++i) {
        const int32_t q = quantizeSigned(e.pos[i] - origin[i], kPosStepM, kPosBitsPerAxis);
        w.writeBits(toOffsetBinary(q, kPosBitsPerAxis), kPosBitsPerAxis);
    }

    // Orientation (smallest-three).
    const SmallestThree st = encodeSmallestThree(e.quat, kQuatBits);
    w.writeBits(st.maxIdx, 2);
    for (int i = 0; i < 3; ++i)
        w.writeBits(st.comp[i], kQuatBits);

    // Velocity.
    for (int i = 0; i < 3; ++i)
        w.writeBits(quantizeRange(static_cast<double>(e.vel[i]), kVelMaxMps, kVelBits), kVelBits);

    // Angular rates (own entity only).
    if (e.hasOmega) {
        for (int i = 0; i < 3; ++i)
            w.writeBits(quantizeRange(static_cast<double>(e.omega[i]), kOmegaMaxRadS, kOmegaBits), kOmegaBits);
    }

    // Packed byte fields.
    w.writeBits(e.damageLevel & static_cast<uint8_t>(bitMask(kDamageBits)), kDamageBits);
    w.writeBits(e.engineFailFlags & static_cast<uint8_t>(bitMask(kEngineFailBits)), kEngineFailBits);
    w.writeBits(e.throttle, kThrottleBits);
    w.writeBits(e.fuelPct, kFuelBits);
    w.writeBits(e.abEngaged ? 1u : 0u, 1);
    w.writeBits(e.playerOwned ? 1u : 0u, 1);

    // Own-record loadout block (#625), gated by the same own-record bit as omega.
    if (e.hasOmega) {
        w.writeBits(e.selectedStation, 8);
        w.writeBits(e.stationRounds, 16);
        w.writeBits(e.weaponFlags, 8);
        const double massClamped = std::min(65535.0, std::max(0.0, static_cast<double>(e.payloadMassKg)));
        w.writeBits(static_cast<uint32_t>(massClamped + 0.5), 16);
        const double cd0Steps = std::min(65535.0, std::max(0.0, static_cast<double>(e.payloadCd0) * 1e5));
        w.writeBits(static_cast<uint32_t>(cd0Steps + 0.5), 16);
    }
}

void encodeStandaloneRecord(std::vector<uint8_t>& out, const QuantEntity& e, const double origin[3], bool sendGen) {
    BitWriter w;
    encodeBody(w, e, origin, sendGen);
    w.alignToByte(); // each record is byte-aligned so it can be stitched by memcpy in any order
    out.insert(out.end(), w.bytes().begin(), w.bytes().end());
}

void appendStitchedRecord(std::vector<uint8_t>& stream, uint32_t originIndex, const std::vector<uint8_t>& blob) {
    // Origin index as LEB128 whole bytes — keeps the stream byte-aligned for the blob that follows.
    while (originIndex >= 0x80u) {
        stream.push_back(static_cast<uint8_t>((originIndex & 0x7Fu) | 0x80u));
        originIndex >>= 7;
    }
    stream.push_back(static_cast<uint8_t>(originIndex));
    stream.insert(stream.end(), blob.begin(), blob.end());
}

bool decodeStandaloneRecord(BitReader& r, QuantEntity& out, const double* originTable, uint32_t originCount,
                            bool& genPresent) {
    uint32_t originIdx = 0;
    if (!r.readVarint(originIdx) || originIdx >= originCount)
        return false;
    const double* origin = originTable + static_cast<std::size_t>(originIdx) * 3u;

    uint32_t idx = 0;
    if (!r.readVarint(idx))
        return false;
    out.idx = idx;

    uint32_t fullBit = 0, genBit = 0, omegaBit = 0;
    if (!r.readBits(1, fullBit) || !r.readBits(1, genBit) || !r.readBits(1, omegaBit))
        return false;
    out.isFull = (fullBit != 0u);
    genPresent = (genBit != 0u);
    out.hasOmega = (omegaBit != 0u);

    if (genPresent) {
        uint32_t g = 0;
        if (!r.readBits(16, g))
            return false;
        out.gen = g;
    }
    if (out.isFull) {
        uint32_t t = 0;
        if (!r.readVarint(t))
            return false;
        out.typeIndex = t;
    }

    // Position (relative to the shared grid origin).
    for (int i = 0; i < 3; ++i) {
        uint32_t u = 0;
        if (!r.readBits(kPosBitsPerAxis, u))
            return false;
        out.pos[i] = origin[i] + dequantizeSigned(fromOffsetBinary(u, kPosBitsPerAxis), kPosStepM);
    }

    // Orientation.
    SmallestThree st;
    if (!r.readBits(2, st.maxIdx))
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!r.readBits(kQuatBits, st.comp[i]))
            return false;
    }
    decodeSmallestThree(st, kQuatBits, out.quat);

    // Velocity.
    for (int i = 0; i < 3; ++i) {
        uint32_t u = 0;
        if (!r.readBits(kVelBits, u))
            return false;
        out.vel[i] = static_cast<float>(dequantizeRange(u, kVelMaxMps, kVelBits));
    }

    // Angular rates.
    if (out.hasOmega) {
        for (int i = 0; i < 3; ++i) {
            uint32_t u = 0;
            if (!r.readBits(kOmegaBits, u))
                return false;
            out.omega[i] = static_cast<float>(dequantizeRange(u, kOmegaMaxRadS, kOmegaBits));
        }
    } else {
        out.omega[0] = out.omega[1] = out.omega[2] = 0.f;
    }

    // Packed byte fields.
    uint32_t dmg = 0, ef = 0, thr = 0, fuel = 0, ab = 0, owned = 0;
    if (!r.readBits(kDamageBits, dmg) || !r.readBits(kEngineFailBits, ef) || !r.readBits(kThrottleBits, thr) ||
        !r.readBits(kFuelBits, fuel) || !r.readBits(1, ab) || !r.readBits(1, owned))
        return false;
    out.damageLevel = static_cast<uint8_t>(dmg);
    out.engineFailFlags = static_cast<uint8_t>(ef);
    out.throttle = static_cast<uint8_t>(thr);
    out.fuelPct = static_cast<uint8_t>(fuel);
    out.abEngaged = (ab != 0u);
    out.playerOwned = (owned != 0u);

    // Own-record loadout block (#625) — present exactly when omega is.
    if (out.hasOmega) {
        uint32_t station = 0, rounds = 0, wflags = 0, mass = 0, cd0 = 0;
        if (!r.readBits(8, station) || !r.readBits(16, rounds) || !r.readBits(8, wflags) || !r.readBits(16, mass) ||
            !r.readBits(16, cd0))
            return false;
        out.selectedStation = static_cast<uint8_t>(station);
        out.stationRounds = static_cast<uint16_t>(rounds);
        out.weaponFlags = static_cast<uint8_t>(wflags);
        out.payloadMassKg = static_cast<float>(mass);
        out.payloadCd0 = static_cast<float>(cd0) * 1e-5f;
    }

    r.alignToByte(); // skip the blob's trailing padding to land on the next record boundary
    return true;
}

namespace {
// Bit cost of an LEB128 varint: 8 bits per 7-bit group, minimum one byte (value 0..127).
uint32_t varintBits(uint32_t value) noexcept {
    uint32_t groups = 1;
    while (value >= 0x80u) {
        value >>= 7;
        ++groups;
    }
    return groups * 8u;
}
} // namespace

uint32_t estimateRecordBytes(bool isFull, bool sendGen, bool hasOmega, uint32_t typeIndex, uint32_t entityIndex,
                             uint32_t originIndex) noexcept {
    uint32_t bits = varintBits(originIndex); // origin index varint (whole bytes)
    bits += varintBits(entityIndex);         // absolute idx varint
    bits += 3;                               // full + genPresent + omegaPresent flag bits
    if (sendGen)
        bits += 16; // gen
    if (isFull)
        bits += varintBits(typeIndex); // typeIndex varint
    bits += 3u * static_cast<uint32_t>(kPosBitsPerAxis);
    bits += 2u + 3u * static_cast<uint32_t>(kQuatBits); // smallest-three: 2-bit index + 3 components
    bits += 3u * static_cast<uint32_t>(kVelBits);
    if (hasOmega)
        bits += 3u * static_cast<uint32_t>(kOmegaBits) + 64u; // + the own-record loadout block (#625)
    bits += static_cast<uint32_t>(kDamageBits + kEngineFailBits + kThrottleBits + kFuelBits) + 2u; // +ab+owned
    return (bits + 7u) / 8u;
}

} // namespace fl
