// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/SnapshotCodec.h"

#include "net/BitStream.h"
#include "net/Quantization.h"

namespace fl {

void encodeRecord(BitWriter& w, const QuantEntity& e, uint32_t& prevIdx, const double origin[3], bool sendGen) {
    // idx delta (records are sorted ascending, so this is non-negative).
    w.writeVarint(e.idx - prevIdx);
    prevIdx = e.idx;

    w.writeBits(e.isFull ? 1u : 0u, 1);
    w.writeBits(sendGen ? 1u : 0u, 1);
    w.writeBits(e.hasOmega ? 1u : 0u, 1);

    if (sendGen)
        w.writeBits(e.gen & 0xFFFFu, 16);
    if (e.isFull)
        w.writeVarint(e.typeIndex);

    // Position relative to frame origin.
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
}

bool decodeRecord(BitReader& r, QuantEntity& out, uint32_t& prevIdx, const double origin[3], bool& genPresent) {
    uint32_t idxDelta = 0;
    if (!r.readVarint(idxDelta))
        return false;
    out.idx = prevIdx + idxDelta;
    prevIdx = out.idx;

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

    // Position.
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
    return true;
}

} // namespace fl
