// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <numbers>

namespace fl {

// Quaternion rotation, in one place (#1248). Convention throughout: q = [x, y, z, w], matching
// EntityTransform::quat.
//
// Three independent bodies existed — FlightIntegrator's, WorldBroadcaster's, and a third inlined
// inside DeckDef's deckLocalPoint. They computed the same rotation and were NOT interchangeable:
// FlightIntegrator pre-doubles the cross product and sums ((v + T1) + T2) - T3, while
// WorldBroadcaster doubled later and summed (v + T1) + 2*(d1 - d2). Multiplying by two is exact in
// IEEE, so the first term always agreed — but the association differs, so the results could differ
// in the last ulp. On the wire that is one quantisation bucket, rarely.
//
// The bodies below are FlightIntegrator's, moved VERBATIM, so the flight model that the determinism
// gate pins stays bit-identical. WorldBroadcaster adopting them is the deliberate part; see #1248.
//
// Header-only and stdlib-only, the Table1D.h precedent: no target, no link edge, and no
// entity->flight or net->flight include story to invent.

// Normalise a quaternion in place. Below the epsilon the quaternion is left alone rather than
// producing a NaN: a zero quaternion is a bug upstream, and turning it into a NaN loses the
// evidence and poisons everything downstream of it.
inline void quatNorm(float* q) {
    float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-6f) {
        q[0] /= len;
        q[1] /= len;
        q[2] /= len;
        q[3] /= len;
    }
}

// Rotate vector v by quaternion q: q * [v, 0] * q^-1.
[[nodiscard]] inline std::array<float, 3> quatRotate(const float* q, const float* v) {
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float vx = v[0], vy = v[1], vz = v[2];
    float tx = 2.f * (qy * vz - qz * vy);
    float ty = 2.f * (qz * vx - qx * vz);
    float tz = 2.f * (qx * vy - qy * vx);
    return {vx + qw * tx + qy * tz - qz * ty, vy + qw * ty + qz * tx - qx * tz, vz + qw * tz + qx * ty - qy * tx};
}

// Double-precision rotation: a float quaternion rotates a double vector. Used for the
// vel_body -> pos_world update, so double velocity precision is not truncated to float before it
// accumulates into the double pos_world fields.
[[nodiscard]] inline std::array<double, 3> quatRotateD(const float* q, const double* v) {
    double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    double vx = v[0], vy = v[1], vz = v[2];
    double tx = 2.0 * (qy * vz - qz * vy);
    double ty = 2.0 * (qz * vx - qx * vz);
    double tz = 2.0 * (qx * vy - qy * vx);
    return {vx + qw * tx + qy * tz - qz * ty, vy + qw * ty + qz * tx - qx * tz, vz + qw * tz + qx * ty - qy * tx};
}

// World -> body: rotate by the CONJUGATE of q, in double. The conjugate is the inverse for a unit
// quaternion, and negating a float is exact, so this is quatRotateD on a negated axis and nothing
// more — which is what the deck-footprint transform has always computed.
[[nodiscard]] inline std::array<double, 3> quatRotateConjD(const float* q, const double* v) {
    const float conj[4] = {-q[0], -q[1], -q[2], q[3]};
    return quatRotateD(conj, v);
}

// Euler angles (roll=x, pitch=y, yaw=z) from a quaternion, ZYX convention. Pitch saturates at
// +/-pi/2 rather than feeding an out-of-domain value to asin at gimbal lock.
[[nodiscard]] inline std::array<float, 3> quatToEuler(const float* q) {
    float sinr_cosp = 2.f * (q[3] * q[0] + q[1] * q[2]);
    float cosr_cosp = 1.f - 2.f * (q[0] * q[0] + q[1] * q[1]);
    float roll = std::atan2(sinr_cosp, cosr_cosp);

    float sinp = 2.f * (q[3] * q[1] - q[2] * q[0]);
    float pitch = std::abs(sinp) >= 1.f ? std::copysign(std::numbers::pi_v<float> / 2.f, sinp) : std::asin(sinp);

    float siny_cosp = 2.f * (q[3] * q[2] + q[0] * q[1]);
    float cosy_cosp = 1.f - 2.f * (q[1] * q[1] + q[2] * q[2]);
    float yaw = std::atan2(siny_cosp, cosy_cosp);

    return {roll, pitch, yaw};
}

} // namespace fl
