// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Quantization math for the snapshot codec (SnapshotCodec.h). Pure arithmetic — no IEEE bit-casts,
// no glm, no BitStream dependency — so encoding is byte-deterministic across compilers/ABIs.
//
// Signed values use OFFSET-BINARY: a signed quantized integer q in [-2^(bits-1), 2^(bits-1)-1] is
// transmitted as the unsigned u = q + 2^(bits-1) in `bits` bits, so the bitstream only ever shifts
// unsigned values (no implementation-defined signed shift). NaN/Inf inputs are clamped to range
// before any float->int cast (out-of-range float->int is UB).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {

// 1/sqrt(2): bound on the three smallest components of a unit quaternion (the largest is dropped).
inline constexpr double kInvSqrt2 = 0.70710678118654752440;

// Clamp to a finite range, mapping NaN to lo (defensive — the integrator should never emit NaN).
inline double clampFinite(double v, double lo, double hi) noexcept {
    if (!(v == v)) // NaN
        return lo;
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

// Quantize a value to a signed integer of `bits` width with resolution `step`, clamped to range.
// bits in [2, 32]. Returns the signed quantized integer.
inline int32_t quantizeSigned(double value, double step, int bits) noexcept {
    const double qmaxF = static_cast<double>((int64_t{1} << (bits - 1)) - 1);
    const double qminF = -static_cast<double>(int64_t{1} << (bits - 1));
    const double scaled = clampFinite(value / step, qminF, qmaxF);
    return static_cast<int32_t>(std::llround(scaled));
}

inline double dequantizeSigned(int32_t q, double step) noexcept {
    return static_cast<double>(q) * step;
}

// Offset-binary transform: signed quantized integer -> unsigned wire value (and back).
inline uint32_t toOffsetBinary(int32_t q, int bits) noexcept {
    return static_cast<uint32_t>(q + (int32_t{1} << (bits - 1))) & static_cast<uint32_t>((uint64_t{1} << bits) - 1);
}
inline int32_t fromOffsetBinary(uint32_t u, int bits) noexcept {
    return static_cast<int32_t>(u) - (int32_t{1} << (bits - 1));
}

// Quantize a value known to lie in [-range, +range] to an unsigned offset-binary code of `bits`.
inline uint32_t quantizeRange(double value, double range, int bits) noexcept {
    const double step = range / static_cast<double>(int64_t{1} << (bits - 1));
    return toOffsetBinary(quantizeSigned(value, step, bits), bits);
}
inline double dequantizeRange(uint32_t u, double range, int bits) noexcept {
    const double step = range / static_cast<double>(int64_t{1} << (bits - 1));
    return dequantizeSigned(fromOffsetBinary(u, bits), step);
}

// ── the articulation and crew-turret wire pairs (#1249) ──────────────────────
//
// These do NOT go through quantizeRange: they are symmetric-scale codes that predate it and are on
// the wire, so their exact expressions are the contract. What they were missing is being a PAIR.
// The encode side lived in engine/net/SnapshotPipeline.cpp and the decode side in
// game/fighters-legacy/ClientNetEventHandler.cpp, a whole target away, kept in step by a comment
// on each side saying the other one existed. A third spelling of the same scale factors sat in the
// decode test's hand-built wire bytes.
//
// Naming them together is the point: an edit to one now has the other in the same screen, and the
// round-trip is testable as a unit instead of only through a live client.

// Angle in [-range, +range] to int16, full-scale at 32767.
[[nodiscard]] inline int16_t quantAngleI16(float a, float range) noexcept {
    const float clamped = std::clamp(a, -range, range);
    return static_cast<int16_t>(std::lround(clamped / range * 32767.f));
}
[[nodiscard]] inline float dequantAngleI16(int16_t q, float range) noexcept {
    return static_cast<float>(q) / 32767.f * range;
}

// Signed articulation channel in [-1, 1] as offset binary around 128.
[[nodiscard]] inline uint8_t quantSignedU8(float v) noexcept {
    const float clamped = std::clamp(v, -1.f, 1.f);
    return static_cast<uint8_t>(std::lround(clamped * 127.f) + 128);
}
[[nodiscard]] inline float dequantSignedU8(uint8_t q) noexcept {
    return static_cast<float>(static_cast<int>(q) - 128) / 127.f;
}

// Unsigned articulation channel in [0, 1] as a plain 0..255 fraction.
[[nodiscard]] inline uint8_t quantUnitU8(float v) noexcept {
    const float clamped = std::clamp(v, 0.f, 1.f);
    return static_cast<uint8_t>(std::lround(clamped * 255.f));
}
[[nodiscard]] inline float dequantUnitU8(uint8_t q) noexcept {
    return static_cast<float>(q) / 255.f;
}

// ---- Smallest-three quaternion encoding ----
// Drop the largest-magnitude component (reconstructed from unit-length), flip the whole quaternion
// so that component is positive (q and -q are the same rotation), and send the index of the dropped
// component (0..3) plus the other three, each in [-1/sqrt2, +1/sqrt2].

struct SmallestThree {
    uint32_t maxIdx{0}; // which component was dropped (0..3 => x,y,z,w)
    uint32_t comp[3]{}; // offset-binary codes of the remaining three, in ascending component order
};

// q layout: [x, y, z, w] (matches EntityTransform::quat). bits = width of each transmitted comp.
inline SmallestThree encodeSmallestThree(const float q[4], int bits) noexcept {
    SmallestThree out;
    int maxIdx = 0;
    double maxAbs = -1.0;
    for (int i = 0; i < 4; ++i) {
        const double a = std::fabs(static_cast<double>(q[i]));
        if (a > maxAbs) {
            maxAbs = a;
            maxIdx = i;
        }
    }
    out.maxIdx = static_cast<uint32_t>(maxIdx);
    const double sign = (q[maxIdx] < 0.f) ? -1.0 : 1.0;
    int w = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == maxIdx)
            continue;
        const double c = sign * static_cast<double>(q[i]); // in [-1/sqrt2, +1/sqrt2]
        out.comp[w++] = quantizeRange(c, kInvSqrt2, bits);
    }
    return out;
}

inline void decodeSmallestThree(const SmallestThree& in, int bits, float qOut[4]) noexcept {
    double c[3];
    double sumSq = 0.0;
    for (int i = 0; i < 3; ++i) {
        c[i] = dequantizeRange(in.comp[i], kInvSqrt2, bits);
        sumSq += c[i] * c[i];
    }
    const double largest = std::sqrt(std::fmax(0.0, 1.0 - sumSq));
    int r = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == static_cast<int>(in.maxIdx))
            qOut[i] = static_cast<float>(largest);
        else
            qOut[i] = static_cast<float>(c[r++]);
    }
    // Renormalise to absorb quantization error.
    double n = 0.0;
    for (int i = 0; i < 4; ++i)
        n += static_cast<double>(qOut[i]) * qOut[i];
    n = std::sqrt(n);
    if (n > 1e-9) {
        const float inv = static_cast<float>(1.0 / n);
        for (int i = 0; i < 4; ++i)
            qOut[i] *= inv;
    } else {
        qOut[0] = qOut[1] = qOut[2] = 0.f;
        qOut[3] = 1.f;
    }
}

} // namespace fl
