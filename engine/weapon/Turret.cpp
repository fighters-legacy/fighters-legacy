// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/Turret.h"

#include <algorithm>
#include <cmath>

namespace fl {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.f * kPi;

// Wrap an angle into (-pi, pi].
[[nodiscard]] float wrapPi(float a) noexcept {
    while (a > kPi)
        a -= kTwoPi;
    while (a <= -kPi)
        a += kTwoPi;
    return a;
}

// Move `cur` toward `target` by at most `maxStep` (>= 0).
[[nodiscard]] float slewToward(float cur, float target, float maxStep) noexcept {
    const float d = target - cur;
    if (std::abs(d) <= maxStep)
        return target;
    return cur + (d > 0.f ? maxStep : -maxStep);
}

} // namespace

void commandTurretMount(TurretState& t, const TurretLimits& lim, const glm::vec3& aimDirMount) noexcept {
    float az = 0.f;
    float el = 0.f;
    turretAimToAzEl(aimDirMount, az, el);
    t.cmdAzRad = std::clamp(az, lim.azMinRad, lim.azMaxRad);
    t.cmdElRad = std::clamp(el, lim.elMinRad, lim.elMaxRad);
}

void commandTurretWorld(TurretState& t, const TurretLimits& lim, const glm::quat& mountRestQuat,
                        const glm::quat& airframeWorldQuat, const glm::vec3& aimDirWorld) noexcept {
    // world -> body -> mount: conj(mount) * conj(airframe) * aimWorld.
    const glm::vec3 aimBody = glm::conjugate(airframeWorldQuat) * aimDirWorld;
    const glm::vec3 aimMount = glm::conjugate(mountRestQuat) * aimBody;
    commandTurretMount(t, lim, aimMount);
}

void stepTurret(TurretState& t, const TurretLimits& lim, float dt) noexcept {
    const float maxStep = std::max(0.f, lim.slewRateRadS * std::max(0.f, dt));

    // Elevation is always a limited arc — slew directly and clamp.
    t.cmdElRad = std::clamp(t.cmdElRad, lim.elMinRad, lim.elMaxRad);
    t.elRad = std::clamp(slewToward(t.elRad, t.cmdElRad, maxStep), lim.elMinRad, lim.elMaxRad);

    // Azimuth: a full-circle ring takes the shortest arc (it can rotate through +/-pi); a limited
    // ring slews directly, because it cannot cross the dead arc behind the trunnion.
    t.cmdAzRad = std::clamp(t.cmdAzRad, lim.azMinRad, lim.azMaxRad);
    const bool fullCircle = (lim.azMaxRad - lim.azMinRad) >= (kTwoPi - 1e-3f);
    if (fullCircle) {
        const float delta = wrapPi(t.cmdAzRad - t.azRad);
        const float step = std::clamp(delta, -maxStep, maxStep);
        t.azRad = wrapPi(t.azRad + step);
    } else {
        t.azRad = std::clamp(slewToward(t.azRad, t.cmdAzRad, maxStep), lim.azMinRad, lim.azMaxRad);
    }
}

glm::vec3 turretWorldDir(const TurretState& t, const glm::quat& mountRestQuat,
                         const glm::quat& airframeWorldQuat) noexcept {
    const glm::vec3 boreMount = turretBoreMount(t.azRad, t.elRad);
    return airframeWorldQuat * (mountRestQuat * boreMount);
}

} // namespace fl
