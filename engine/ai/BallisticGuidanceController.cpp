// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/BallisticGuidanceController.h"

#include "ai/Guidance.h"
#include "flight/LocalFrame.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl::ai {

namespace {

// Vacuum flat-tangent impact prediction from the current state: how far downrange the vehicle
// lands if the motor quit right now. Deliberately simple — drag and the 1/r² field perturb it by
// a few percent, and the feedback law only needs the SIGN and rough size of the overshoot.
double predictImpactDistM(const glm::dvec3& pos, const glm::vec3& velWorld, double planetRadiusM) {
    constexpr double g = 9.80665;
    const glm::vec3 up = fl::radialUp(pos, planetRadiusM);
    const double vUp = static_cast<double>(glm::dot(velWorld, up));
    const glm::vec3 vHvec = velWorld - up * static_cast<float>(vUp);
    const double vH = static_cast<double>(glm::length(vHvec));
    const double h = std::max(0.0, fl::localAltitude(pos, planetRadiusM));
    const double disc = vUp * vUp + 2.0 * g * h;
    const double tFall = (vUp + std::sqrt(std::max(0.0, disc))) / g;
    return vH * tFall;
}

} // namespace

fl::ControlInput BallisticGuidanceController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                                     const fl::AiTickContext& /*ctx*/) {
    fl::ControlInput ctrl{};
    ctrl.throttle = 1.f; // a solid motor ignores it; stated for honesty

    const glm::dvec3 pos{state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]};
    const glm::vec3 velWorld{state.transform.vel[0], state.transform.vel[1], state.transform.vel[2]};

    // Heading: hold the local-tangent bearing to the impact point. Yaw authority is TVC (rudder).
    const double tgt[3] = {m_p.targetPos.x, m_p.targetPos.y, m_p.targetPos.z};
    const float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgt, m_planetRadiusM);
    ctrl.rudder = std::clamp(headErr * (2.f / std::numbers::pi_v<float>), -1.f, 1.f);
    ctrl.aileron = 0.f; // no roll program

    // Pitch program. THE KEY FACT: with no wings and no cutoff, horizontal velocity once gained is
    // never given back — climbing steeper only ADDS range (a longer fall multiplies the vH you
    // already have). So range control must act EARLY: the command ramps from the loft floor (45°,
    // maximum range growth) toward near-vertical as the continuously-predicted impact point walks
    // out to the target, so the remaining burn adds mostly vertical energy and the predicted range
    // creeps rather than sails. An undershooting shot stays at the floor and honestly lands short.
    const glm::dvec3 toTgt = m_p.targetPos - pos;
    const glm::vec3 up = fl::radialUp(pos, m_planetRadiusM);
    const glm::dvec3 toTgtTangent = toTgt - glm::dvec3(up) * glm::dot(toTgt, glm::dvec3(up));
    const double distToGo = glm::length(toTgtTangent);
    const double predicted = predictImpactDistM(pos, velWorld, m_planetRadiusM);
    // The prediction reads the CURRENT velocity, so during the burn it badly under-reports where
    // the shot will finally land — a linear ramp steepens far too late and the horizontal velocity
    // is already banked. The cube-root squash anticipates: at 3% of the target range the command is
    // already ~2/3 of the way to vertical, and the last knots of horizontal speed arrive slowly.
    const double ratio = distToGo > 1.0 ? std::pow(std::clamp(predicted / distToGo, 0.0, 1.0), 1.0 / 3.0) : 1.0;

    float pitchCmdDeg = m_p.basePitchDeg + static_cast<float>(ratio) * (88.f - m_p.basePitchDeg);
    // A genuine overshoot (short target, energetic booster) pushes the command PAST vertical: the
    // thrust vector gains a retrograde horizontal component and actively takes banked horizontal
    // speed back — the only way a fixed-impulse motor can hit inside its natural minimum range.
    if (distToGo > 1.0 && predicted > distToGo)
        pitchCmdDeg += static_cast<float>(std::min((predicted - distToGo) / distToGo, 1.0)) * 30.f;

    const float pitchNow = fl::pitchOf(state.transform.quat, pos, m_planetRadiusM);
    const float pitchErr = glm::radians(pitchCmdDeg) - pitchNow;
    ctrl.elevator = std::clamp(pitchErr * 2.f, -1.f, 1.f);

    // MIRV deploy (#355): once, past apogee (descending in the local frame). Children fan out on
    // deterministic velocity nudges — a fixed ring, no dice — and inherit this controller with no
    // MIRV of their own. Ownership chains in WorldBroadcaster's drain, not here.
    if (m_p.mirvCount > 0 && !m_mirvDeployed) {
        const float vUpNow = glm::dot(velWorld, up);
        if (vUpNow < 0.f) {
            m_mirvDeployed = true;
            // A tangent basis for the fan: east-ish and the downrange direction.
            glm::vec3 fwd = distToGo > 1.0 ? glm::normalize(glm::vec3(toTgtTangent)) : glm::vec3{1.f, 0.f, 0.f};
            const glm::vec3 side = glm::normalize(glm::cross(up, fwd));
            const double tFall = std::max(1.0, predictImpactDistM(pos, velWorld, m_planetRadiusM) /
                                                   std::max(1.0, static_cast<double>(glm::length(velWorld))));
            const float nudge = static_cast<float>(m_p.mirvSpreadM / tFall);
            for (int i = 0; i < m_p.mirvCount; ++i) {
                const float ang =
                    static_cast<float>(i) * 2.f * std::numbers::pi_v<float> / static_cast<float>(m_p.mirvCount);
                const glm::vec3 childVel = velWorld + (fwd * std::cos(ang) + side * std::sin(ang)) * nudge;

                fl::SpawnRequest req;
                req.typeId = m_p.mirvTypeId; // empty = same type as the bus (resolved at drain)
                for (int a = 0; a < 3; ++a) {
                    req.transform.pos[a] = state.transform.pos[a];
                    req.transform.vel[a] = childVel[a];
                }
                for (int a = 0; a < 4; ++a)
                    req.transform.quat[a] = state.transform.quat[a];
                Params childParams;
                childParams.targetPos = m_p.targetPos;
                childParams.basePitchDeg = m_p.basePitchDeg;
                childParams.mirvCount = 0; // an RV does not MIRV
                req.makeController = [childParams]() -> std::unique_ptr<fl::IEntityController> {
                    return std::make_unique<BallisticGuidanceController>(childParams);
                };
                m_pending.push_back(std::move(req));
            }
        }
    }

    return ctrl;
}

} // namespace fl::ai
