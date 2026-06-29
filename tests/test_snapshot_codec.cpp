// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/BitStream.h"
#include "net/Quantization.h"
#include "net/SnapshotCodec.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Approx;

namespace {

// Decode helper: run encode then decode of a single record and return the decoded entity.
fl::QuantEntity roundTrip(const fl::QuantEntity& in, const double origin[3], bool sendGen, bool& genPresentOut) {
    fl::BitWriter w;
    uint32_t prevW = 0;
    fl::encodeRecord(w, in, prevW, origin, sendGen);
    w.alignToByte();

    fl::BitReader r(w.bytes().data(), w.byteCount());
    fl::QuantEntity out;
    uint32_t prevR = 0;
    REQUIRE(fl::decodeRecord(r, out, prevR, origin, genPresentOut));
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// BitStream primitives
// ---------------------------------------------------------------------------------------------

TEST_CASE("BitStream: round-trips fields straddling byte boundaries", "[snapshot_codec][bitstream]") {
    fl::BitWriter w;
    w.writeBits(0x5u, 3);         // 101
    w.writeBits(0x1ABCu, 13);     // 13-bit value
    w.writeBits(0x00ABCDEFu, 24); // 24-bit value crossing several bytes
    w.alignToByte();

    fl::BitReader r(w.bytes().data(), w.byteCount());
    uint32_t a = 0, b = 0, c = 0;
    REQUIRE(r.readBits(3, a));
    REQUIRE(r.readBits(13, b));
    REQUIRE(r.readBits(24, c));
    CHECK(a == 0x5u);
    CHECK(b == 0x1ABCu);
    CHECK(c == 0x00ABCDEFu);
}

TEST_CASE("BitStream: max-width 32-bit writes round-trip", "[snapshot_codec][bitstream]") {
    fl::BitWriter w;
    w.writeBits(0xFFFFFFFFu, 32);
    w.writeBits(0x12345678u, 32);
    w.alignToByte();

    fl::BitReader r(w.bytes().data(), w.byteCount());
    uint32_t a = 0, b = 0;
    REQUIRE(r.readBits(32, a));
    REQUIRE(r.readBits(32, b));
    CHECK(a == 0xFFFFFFFFu);
    CHECK(b == 0x12345678u);
}

TEST_CASE("BitStream: varint round-trips small and large values", "[snapshot_codec][bitstream]") {
    const uint32_t values[] = {0u, 1u, 127u, 128u, 16383u, 16384u, 0xFFFFFFFFu};
    for (uint32_t v : values) {
        fl::BitWriter w;
        w.writeVarint(v);
        w.alignToByte();
        fl::BitReader r(w.bytes().data(), w.byteCount());
        uint32_t out = 0;
        REQUIRE(r.readVarint(out));
        CHECK(out == v);
    }
}

TEST_CASE("BitStream: alignToByte pads to whole bytes", "[snapshot_codec][bitstream]") {
    fl::BitWriter w;
    w.writeBits(0x1u, 1);
    CHECK(w.byteCount() == 0u); // nothing flushed yet
    w.alignToByte();
    CHECK(w.byteCount() == 1u);
}

TEST_CASE("BitStream: truncated buffer fails closed (no OOB read)", "[snapshot_codec][bitstream]") {
    fl::BitWriter w;
    w.writeBits(0xABCDu, 16);
    w.alignToByte();
    // Hand the reader only the first byte of a 2-byte value, then ask for 16 bits.
    fl::BitReader r(w.bytes().data(), 1u);
    uint32_t out = 0;
    CHECK_FALSE(r.readBits(16, out));
}

// ---------------------------------------------------------------------------------------------
// Quantization
// ---------------------------------------------------------------------------------------------

TEST_CASE("Quantization: signed range round-trip within half a step", "[snapshot_codec][quant]") {
    const double range = 2000.0;
    const int bits = 14;
    const double step = range / static_cast<double>(1 << (bits - 1));
    for (double v : {0.0, 12.5, -350.0, 1999.0, -1999.0}) {
        const uint32_t u = fl::quantizeRange(v, range, bits);
        const double back = fl::dequantizeRange(u, range, bits);
        CHECK(std::fabs(back - v) <= step / 2.0 + 1e-9);
    }
}

TEST_CASE("Quantization: clamps beyond-range and NaN without UB", "[snapshot_codec][quant]") {
    const double range = 20.0;
    const int bits = 12;
    const double maxV = fl::dequantizeRange(fl::quantizeRange(1e9, range, bits), range, bits);
    const double minV = fl::dequantizeRange(fl::quantizeRange(-1e9, range, bits), range, bits);
    CHECK(maxV <= range);
    CHECK(minV >= -range);
    CHECK(maxV == Approx(range).margin(range / (1 << (bits - 1))));
    // NaN maps to the low end of the clamp range, deterministically.
    const double nanV = fl::dequantizeRange(fl::quantizeRange(std::nan(""), range, bits), range, bits);
    CHECK(nanV >= -range);
    CHECK(nanV <= range);
}

TEST_CASE("Quantization: smallest-three handles each largest component", "[snapshot_codec][quant]") {
    // Quaternions whose largest-magnitude component is x, y, z, w respectively.
    const float quats[4][4] = {
        {0.97f, 0.10f, 0.10f, 0.18f}, // x largest
        {0.10f, 0.97f, 0.10f, 0.18f}, // y largest
        {0.10f, 0.10f, 0.97f, 0.18f}, // z largest
        {0.18f, 0.10f, 0.10f, 0.97f}, // w largest
    };
    for (auto& src : quats) {
        // normalise input
        float q[4];
        double n = 0;
        for (int i = 0; i < 4; ++i)
            n += static_cast<double>(src[i]) * src[i];
        n = std::sqrt(n);
        for (int i = 0; i < 4; ++i)
            q[i] = static_cast<float>(src[i] / n);

        fl::SmallestThree st = fl::encodeSmallestThree(q, 10);
        float dq[4];
        fl::decodeSmallestThree(st, 10, dq);

        // decoded is unit-length
        double dn = 0;
        for (int i = 0; i < 4; ++i)
            dn += static_cast<double>(dq[i]) * dq[i];
        CHECK(dn == Approx(1.0).margin(1e-3));

        // same rotation: |dot| ~ 1 (sign-insensitive)
        double dot = 0;
        for (int i = 0; i < 4; ++i)
            dot += static_cast<double>(dq[i]) * q[i];
        CHECK(std::fabs(dot) == Approx(1.0).margin(2e-3));
    }
}

TEST_CASE("Quantization: identity quaternion round-trips", "[snapshot_codec][quant]") {
    const float id[4] = {0.f, 0.f, 0.f, 1.f};
    fl::SmallestThree st = fl::encodeSmallestThree(id, 10);
    float dq[4];
    fl::decodeSmallestThree(st, 10, dq);
    CHECK(dq[0] == Approx(0.f).margin(2e-3));
    CHECK(dq[1] == Approx(0.f).margin(2e-3));
    CHECK(dq[2] == Approx(0.f).margin(2e-3));
    CHECK(std::fabs(dq[3]) == Approx(1.f).margin(2e-3));
}

TEST_CASE("Quantization: negative largest component reconstructs sign-equivalent rotation", "[snapshot_codec][quant]") {
    float q[4] = {0.1f, -0.2f, 0.1f, -0.967f}; // w largest and negative
    double n = 0;
    for (int i = 0; i < 4; ++i)
        n += static_cast<double>(q[i]) * q[i];
    n = std::sqrt(n);
    for (int i = 0; i < 4; ++i)
        q[i] = static_cast<float>(q[i] / n);

    fl::SmallestThree st = fl::encodeSmallestThree(q, 10);
    float dq[4];
    fl::decodeSmallestThree(st, 10, dq);
    double dot = 0;
    for (int i = 0; i < 4; ++i)
        dot += static_cast<double>(dq[i]) * q[i];
    CHECK(std::fabs(dot) == Approx(1.0).margin(3e-3));
}

// ---------------------------------------------------------------------------------------------
// SnapshotCodec records
// ---------------------------------------------------------------------------------------------

TEST_CASE("SnapshotCodec: full record round-trips all fields", "[snapshot_codec]") {
    const double origin[3] = {1000.0, 500.0, -2000.0};
    fl::QuantEntity in;
    in.idx = 7;
    in.gen = 3;
    in.typeIndex = 42;
    in.isFull = true;
    in.hasOmega = true;
    in.pos[0] = 1000.0 + 12.5;
    in.pos[1] = 500.0 - 3.0;
    in.pos[2] = -2000.0 + 250.0;
    in.vel[0] = 120.0f;
    in.vel[1] = -8.0f;
    in.vel[2] = 33.0f;
    in.quat[0] = 0.f;
    in.quat[1] = 0.f;
    in.quat[2] = 0.7071f;
    in.quat[3] = 0.7071f;
    in.omega[0] = 0.5f;
    in.omega[1] = -1.2f;
    in.omega[2] = 0.05f;
    in.damageLevel = 2;
    in.engineFailFlags = 0x10;
    in.throttle = 88;
    in.fuelPct = 47;
    in.abEngaged = true;
    in.playerOwned = true;

    bool genPresent = false;
    fl::QuantEntity out = roundTrip(in, origin, /*sendGen=*/true, genPresent);

    CHECK(genPresent);
    CHECK(out.idx == 7u);
    CHECK(out.gen == 3u);
    CHECK(out.isFull);
    CHECK(out.typeIndex == 42u);
    CHECK(out.hasOmega);
    CHECK(out.pos[0] == Approx(in.pos[0]).margin(fl::kPosStepM));
    CHECK(out.pos[1] == Approx(in.pos[1]).margin(fl::kPosStepM));
    CHECK(out.pos[2] == Approx(in.pos[2]).margin(fl::kPosStepM));
    CHECK(out.vel[0] == Approx(in.vel[0]).margin(0.3));
    CHECK(out.vel[2] == Approx(in.vel[2]).margin(0.3));
    CHECK(out.omega[1] == Approx(in.omega[1]).margin(0.02));
    CHECK(out.damageLevel == 2u);
    CHECK(out.engineFailFlags == 0x10u);
    CHECK(out.throttle == 88u);
    CHECK(out.fuelPct == 47u);
    CHECK(out.abEngaged);
    CHECK(out.playerOwned);
}

TEST_CASE("SnapshotCodec: delta record omits gen/type/omega", "[snapshot_codec]") {
    const double origin[3] = {0.0, 0.0, 0.0};
    fl::QuantEntity in;
    in.idx = 5;
    in.isFull = false;
    in.hasOmega = false;
    in.pos[0] = 100.0;
    in.omega[0] = 9.9f; // must NOT be transmitted

    bool genPresent = true;
    fl::QuantEntity out = roundTrip(in, origin, /*sendGen=*/false, genPresent);

    CHECK_FALSE(genPresent);
    CHECK_FALSE(out.isFull);
    CHECK_FALSE(out.hasOmega);
    CHECK(out.pos[0] == Approx(100.0).margin(fl::kPosStepM));
    CHECK(out.omega[0] == Approx(0.f).margin(1e-6)); // absent -> zeroed
}

TEST_CASE("SnapshotCodec: idx delta varint chains across multiple records", "[snapshot_codec]") {
    const double origin[3] = {0.0, 0.0, 0.0};
    const uint32_t ids[] = {3u, 4u, 260u, 5000u};
    fl::BitWriter w;
    uint32_t prevW = 0;
    for (uint32_t id : ids) {
        fl::QuantEntity e;
        e.idx = id;
        fl::encodeRecord(w, e, prevW, origin, /*sendGen=*/false);
    }
    w.alignToByte();

    fl::BitReader r(w.bytes().data(), w.byteCount());
    uint32_t prevR = 0;
    for (uint32_t id : ids) {
        fl::QuantEntity out;
        bool gp = false;
        REQUIRE(fl::decodeRecord(r, out, prevR, origin, gp));
        CHECK(out.idx == id);
    }
}

TEST_CASE("SnapshotCodec: planet-scale frame origin preserves position precision", "[snapshot_codec]") {
    const double origin[3] = {6371000.0, 200000.0, -6371000.0};
    fl::QuantEntity in;
    in.idx = 1;
    in.pos[0] = origin[0] + 15.25;
    in.pos[1] = origin[1] - 42.5;
    in.pos[2] = origin[2] + 1000.0;

    bool gp = false;
    fl::QuantEntity out = roundTrip(in, origin, /*sendGen=*/false, gp);
    CHECK(out.pos[0] == Approx(in.pos[0]).margin(fl::kPosStepM));
    CHECK(out.pos[1] == Approx(in.pos[1]).margin(fl::kPosStepM));
    CHECK(out.pos[2] == Approx(in.pos[2]).margin(fl::kPosStepM));
}

TEST_CASE("SnapshotCodec: deterministic encoding and locked byte sizes", "[snapshot_codec]") {
    const double origin[3] = {0.0, 0.0, 0.0};
    fl::QuantEntity full;
    full.idx = 10;
    full.gen = 1;
    full.typeIndex = 5;
    full.isFull = true;
    full.hasOmega = true;

    fl::BitWriter w1, w2;
    uint32_t p1 = 0, p2 = 0;
    fl::encodeRecord(w1, full, p1, origin, true);
    fl::encodeRecord(w2, full, p2, origin, true);
    w1.alignToByte();
    w2.alignToByte();
    // Deterministic: identical bytes (locks the wire layout against accidental drift).
    CHECK(w1.bytes() == w2.bytes());
    // Locked size for this exact field set (idxDelta=8b + flags=3 + gen=16 + type=8 + pos=66 +
    // quat=32 + vel=54 + omega=36 + bytefields=24 = 247 bits => 31 bytes).
    CHECK(w1.byteCount() == 31u);

    fl::QuantEntity delta;
    delta.idx = 11;
    delta.isFull = false;
    delta.hasOmega = false;
    fl::BitWriter wd;
    uint32_t pd = 0;
    fl::encodeRecord(wd, delta, pd, origin, false);
    wd.alignToByte();
    // Steady-state delta: 8 + 3 + 66 + 32 + 54 + 24 = 187 bits => 24 bytes.
    CHECK(wd.byteCount() == 24u);
}

TEST_CASE("SnapshotCodec: bandwidth guard - quantized records beat the old 64-byte encoding", "[snapshot_codec]") {
    const double origin[3] = {0.0, 0.0, 0.0};
    const int kCount = 32;
    fl::BitWriter w;
    uint32_t prev = 0;
    for (int i = 0; i < kCount; ++i) {
        fl::QuantEntity e;
        e.idx = static_cast<uint32_t>(i + 1);
        e.isFull = false; // steady-state delta
        e.pos[0] = i * 10.0;
        fl::encodeRecord(w, e, prev, origin, false);
    }
    w.alignToByte();
    // The old wire used a fixed 64-byte MsgEntityUpdate per entity.
    const std::size_t oldBytes = static_cast<std::size_t>(kCount) * 64u;
    CHECK(w.byteCount() < oldBytes);
    // Per-record average comfortably under a 28-byte bound (locks the bandwidth win).
    CHECK(w.byteCount() <= static_cast<std::size_t>(kCount) * 28u);
}

TEST_CASE("SnapshotCodec: truncated record decode fails closed", "[snapshot_codec]") {
    const double origin[3] = {0.0, 0.0, 0.0};
    fl::QuantEntity in;
    in.idx = 1;
    in.isFull = true;
    in.typeIndex = 9;
    fl::BitWriter w;
    uint32_t prevW = 0;
    fl::encodeRecord(w, in, prevW, origin, true);
    w.alignToByte();

    // Feed only the first few bytes: decode must return false, not crash or read OOB.
    fl::BitReader r(w.bytes().data(), 3u);
    fl::QuantEntity out;
    uint32_t prevR = 0;
    bool gp = false;
    CHECK_FALSE(fl::decodeRecord(r, out, prevR, origin, gp));
}
