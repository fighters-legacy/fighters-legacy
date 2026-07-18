// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace fl {

// ── Turret mounts (#966/#970) ────────────────────────────────────────────────
//
// A turret gives a weapon station an aiming direction independent of the airframe nose: a
// server-authoritative slew servo that tracks a commanded direction within traverse limits, and a
// launch/fire vector derived from the turret orientation. This is the primitive a defensive gunner
// needs — and it is the ground-SAM launcher-elevation gap documented as owed to #585 (a static
// emplacement mounts its launcher as a turret and slews it via this servo).
//
// The math here is PURE: no world access, no allocation, deterministic. WorldBroadcaster steps it in
// the integrate pass (same worker as the airframe), and the client replays it verbatim as the turret
// predictor — the same reason evaluateFire is pure. TurretLimits carries only radians, so this header
// does not depend on the authored TurretDef (engine-entity); the caller converts once.
//
// FRAME. The mount has a rest orientation (a body-frame quat). In that rest frame the bore is +X, up
// is +Y, right is +Z (the engine's forward/up/right convention). Azimuth rotates about mount up (+Y),
// elevation about the azimuth-rotated right axis, so the bore is Ry(az)·Rz(el)·(+X):
//     bore_mount(az, el) = (cos el·cos az, sin el, -cos el·sin az)
// hence az > 0 sweeps the bore toward -Z (left) and el > 0 raises it toward +Y (up). World bore is
// airframeWorldQuat · mountRestQuat · bore_mount.

// The turret's authored traverse envelope + servo rate, in RADIANS (converted from the degrees in
// TurretDef by the caller). Defaults span a full-circle azimuth ring.
struct TurretLimits {
    float azMinRad{-3.14159265f};
    float azMaxRad{3.14159265f};
    float elMinRad{-0.0872665f};    // -5 deg
    float elMaxRad{1.4835299f};     // +85 deg
    float slewRateRadS{1.0471976f}; // 60 deg/s
};

// The turret's live pose: current azimuth/elevation (mount frame, radians) plus the commanded target
// the servo is slewing toward (already clamped to limits). Lives per-mount inside CrewState (#969).
struct TurretState {
    float azRad{0.f};
    float elRad{0.f};
    float cmdAzRad{0.f};
    float cmdElRad{0.f};
};

// The bore unit vector in the MOUNT rest frame for a given (az, el).
[[nodiscard]] glm::vec3 turretBoreMount(float azRad, float elRad) noexcept;

// Decompose a mount-frame aim direction into (az, el). el is clamped to [-pi/2, pi/2] by asin; az is
// the atan2 in (-pi, pi]. A near-zero direction yields (0, 0).
void turretAimToAzEl(const glm::vec3& aimDirMount, float& outAzRad, float& outElRad) noexcept;

// Set the commanded target from a mount-frame aim direction, clamped to the traverse limits.
void commandTurretMount(TurretState& t, const TurretLimits& lim, const glm::vec3& aimDirMount) noexcept;

// Set the commanded target from a WORLD-space aim direction, converting world -> body -> mount with
// the airframe and mount orientations, then clamping. The convenient server/controller entry point.
void commandTurretWorld(TurretState& t, const TurretLimits& lim, const glm::quat& mountRestQuat,
                        const glm::quat& airframeWorldQuat, const glm::vec3& aimDirWorld) noexcept;

// Slew the pose toward the commanded target at the servo rate, clamped to limits. Pure and
// deterministic. Azimuth takes the shortest arc only for a FULL-circle ring (span >= 2*pi); a
// limited-traverse turret slews directly (it cannot cross the dead arc behind it).
void stepTurret(TurretState& t, const TurretLimits& lim, float dt) noexcept;

// The turret's current world-space bore direction: airframeWorldQuat · mountRestQuat · bore_mount.
[[nodiscard]] glm::vec3 turretWorldDir(const TurretState& t, const glm::quat& mountRestQuat,
                                       const glm::quat& airframeWorldQuat) noexcept;

} // namespace fl
