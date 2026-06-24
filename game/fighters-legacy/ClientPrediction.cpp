// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientPrediction.h"

#include "RenderTypes.h"       // EnvironmentState
#include "flight/AeroForces.h" // ControlInput
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelData.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace fl {

namespace {
static constexpr float kPredTickDt = 1.f / 60.f;

// Rotate a world-frame velocity vector into body frame (quaternion conjugate).
// q: glm::quat with [x,y,z,w] internal layout.
static glm::vec3 worldToBody(const glm::quat& q, glm::vec3 vWorld) {
    return glm::conjugate(q) * vWorld;
}

// Rotate a body-frame velocity vector into world frame.
static glm::vec3 bodyToWorld(const glm::quat& q, glm::vec3 vBody) {
    return q * vBody;
}

// Build a FlightState from an EntityRenderEntry + flight model for mass/fuel defaults.
static FlightState entryToFlightState(const EntityRenderEntry& e, const FlightModelData& model) {
    FlightState fs;

    fs.pos_world[0] = e.position.x;
    fs.pos_world[1] = e.position.y;
    fs.pos_world[2] = e.position.z;

    // Quaternion: glm::quat internal layout is [x,y,z,w]; FlightState quat is [x,y,z,w].
    fs.quat[0] = e.orientation.x;
    fs.quat[1] = e.orientation.y;
    fs.quat[2] = e.orientation.z;
    fs.quat[3] = e.orientation.w;

    // World velocity → body frame
    const glm::vec3 vBody = worldToBody(e.orientation, e.velocity);
    fs.vel_body[0] = static_cast<double>(vBody.x);
    fs.vel_body[1] = static_cast<double>(vBody.y);
    fs.vel_body[2] = static_cast<double>(vBody.z);

    // Angular rates from omega wire field
    fs.omega[0] = e.omega.x;
    fs.omega[1] = e.omega.y;
    fs.omega[2] = e.omega.z;

    fs.fuel_kg = (e.fuelPct / 100.f) * model.geometry.fuel_kg;
    fs.mass_kg = model.geometry.mass_kg + fs.fuel_kg;
    fs.throttle_actual = e.throttle / 100.f;
    fs.current_sweep_deg = model.wing_sweep ? model.wing_sweep->ref_sweep_deg : 0.f;
    fs.ab_engaged = e.abEngaged;
    fs.engineFailFlags = e.engineFailFlags;
    // euler recomputed from quat inside FlightIntegrator::step; tvc_angle_deg stays 0

    return fs;
}

// Overwrite an EntityRenderEntry from a predicted FlightState.
// maxFuel: model.geometry.fuel_kg — needed to convert fuel_kg back to 0-100 percent.
static void stateToEntry(const FlightState& fs, float maxFuel, EntityRenderEntry& out) {
    out.position = {fs.pos_world[0], fs.pos_world[1], fs.pos_world[2]};

    // glm::quat constructor is (w,x,y,z); FlightState quat is [x,y,z,w]
    const glm::quat q(fs.quat[3], fs.quat[0], fs.quat[1], fs.quat[2]);
    out.orientation = q;

    const glm::vec3 vBody(static_cast<float>(fs.vel_body[0]), static_cast<float>(fs.vel_body[1]),
                          static_cast<float>(fs.vel_body[2]));
    out.velocity = bodyToWorld(q, vBody);

    out.omega = {fs.omega[0], fs.omega[1], fs.omega[2]};
    out.throttle = static_cast<uint8_t>(std::clamp(fs.throttle_actual * 100.f, 0.f, 100.f));
    const float fuelPctF = (maxFuel > 0.f) ? fs.fuel_kg / maxFuel * 100.f : 0.f;
    out.fuelPct = static_cast<uint8_t>(std::clamp(fuelPctF, 0.f, 100.f));
    out.abEngaged = fs.ab_engaged;
    out.engineFailFlags = fs.engineFailFlags;
}
} // namespace

ClientPrediction::~ClientPrediction() = default;

void ClientPrediction::init(PredictionSettings cfg, FlightModelResolver resolver, HeightQuery heightQuery,
                            uint32_t playerIdx, uint32_t playerGen, float planetRadiusKm) {
    reset();
    m_cfg = cfg;
    m_resolver = std::move(resolver);
    m_heightQuery = std::move(heightQuery);
    m_playerIdx = playerIdx;
    m_playerGen = playerGen;
    m_planetRadiusKm = planetRadiusKm;
}

void ClientPrediction::reset() {
    m_initialized = false;
    m_integrator.reset();
    m_model.reset();
    m_customGravity.reset();
    m_histHead = 0;
    m_histCount = 0;
    m_hasPrevPrediction = false;
    for (auto& e : m_history) {
        e = HistoryEntry{};
    }
}

void ClientPrediction::pushHistory(uint32_t seqNum, const BufferedInput& bi) noexcept {
    const uint32_t writeIdx = (m_histHead + m_histCount) % kHistorySize;
    m_history[writeIdx] = {seqNum, bi};
    if (m_histCount < kHistorySize) {
        ++m_histCount;
    } else {
        // Ring full — drop oldest
        m_histHead = (m_histHead + 1u) % kHistorySize;
    }
}

uint32_t ClientPrediction::tailHistory(uint32_t count, HistoryEntry* out) const noexcept {
    const uint32_t available = std::min(count, m_histCount);
    // Most-recent `available` entries start at ring index (head + count - available)
    const uint32_t startIdx = (m_histHead + m_histCount - available) % kHistorySize;
    for (uint32_t i = 0; i < available; ++i) {
        out[i] = m_history[(startIdx + i) % kHistorySize];
    }
    return available;
}

void ClientPrediction::stepIntegrator(const BufferedInput& bi, const EnvironmentState& env) {
    if (!m_integrator) {
        return;
    }
    ControlInput ctrl;
    ctrl.throttle = bi.throttle;
    ctrl.elevator = bi.elevator;
    ctrl.aileron = bi.aileron;
    ctrl.rudder = bi.rudder;
    ctrl.afterburner = (bi.buttons & 0x02u) != 0;

    // Steady wind only — turbulence is stochastic on the server and cannot be replicated
    // without a shared seed. Excluding it prevents compound prediction divergence.
    WindInfluence wind;
    wind.wind_world[0] = env.windX;
    wind.wind_world[1] = 0.f;
    wind.wind_world[2] = env.windZ;

    const float groundElev =
        m_heightQuery ? m_heightQuery(m_integrator->state().pos_world[0], m_integrator->state().pos_world[2]) : 0.f;
    m_integrator->step(kPredTickDt, ctrl, {}, wind, groundElev);
}

void ClientPrediction::onInput(const MsgClientInput& msg, const EnvironmentState& env) {
    if (!m_cfg.enabled) {
        return;
    }
    BufferedInput bi;
    bi.throttle = std::clamp(msg.throttle, 0.f, 1.f);
    bi.elevator = std::clamp(msg.elevator, -1.f, 1.f);
    bi.aileron = std::clamp(msg.aileron, -1.f, 1.f);
    bi.rudder = std::clamp(msg.rudder, -1.f, 1.f);
    bi.buttons = msg.buttons;

    pushHistory(msg.seqNum, bi);

    if (m_initialized) {
        stepIntegrator(bi, env);
    }
}

void ClientPrediction::reconcile(RenderSnapshot& snap, uint64_t /*tickIndex*/, uint32_t estimatedDelayTicks,
                                 const EnvironmentState& env) {
    if (!m_cfg.enabled) {
        return;
    }

    // Find the player's entry.
    EntityRenderEntry* playerEntry = nullptr;
    for (auto& e : snap.entries) {
        if (e.entityIdx == m_playerIdx && e.entityGen == m_playerGen) {
            playerEntry = &e;
            break;
        }
    }
    if (!playerEntry) {
        return;
    }

    // Lazy init: resolve the flight model on the first snapshot with the player's entry.
    if (!m_initialized) {
        m_model = m_resolver ? m_resolver(playerEntry->typeIndex) : nullptr;
        if (!m_model) {
            m_model = BuiltinFlightModel::get();
        }
        m_integrator = std::make_unique<FlightIntegrator>(m_model);
        // Match the server's gravity field for non-Earth planet radii.
        if (std::abs(m_planetRadiusKm - 6371.f) > 1.f) {
            m_customGravity.emplace(m_planetRadiusKm * 1000.f);
            m_integrator->setGravityField(*m_customGravity);
        }
        m_initialized = true;
    }

    // Reset integrator to the server's authoritative state.
    const FlightState serverState = entryToFlightState(*playerEntry, *m_model);
    m_integrator->reset(serverState);

    // Replay the last estimatedDelayTicks inputs, oldest-first.
    const uint32_t replayCount = std::min(estimatedDelayTicks, m_histCount);
    if (replayCount > 0) {
        HistoryEntry replayBuf[kHistorySize];
        const uint32_t got = tailHistory(replayCount, replayBuf);
        for (uint32_t i = 0; i < got; ++i) {
            stepIntegrator(replayBuf[i].input, env);
        }
    }

    const glm::dvec3 newPredPos = {m_integrator->state().pos_world[0], m_integrator->state().pos_world[1],
                                   m_integrator->state().pos_world[2]};

    // Snap vs. blend: if previous prediction is available and divergence is within the
    // soft-correction threshold, blend the position toward the newly predicted value.
    if (m_hasPrevPrediction && m_cfg.snapThresholdM > 0.f) {
        const glm::dvec3 delta = newPredPos - m_lastPredPos;
        const float dist = static_cast<float>(std::sqrt(glm::dot(delta, delta)));
        if (dist <= m_cfg.snapThresholdM && m_cfg.blendRate < 1.f) {
            const float t = m_cfg.blendRate;
            const glm::dvec3 blended = m_lastPredPos + glm::dvec3(t) * (newPredPos - m_lastPredPos);
            FlightState blendedState = m_integrator->state();
            blendedState.pos_world[0] = blended.x;
            blendedState.pos_world[1] = blended.y;
            blendedState.pos_world[2] = blended.z;
            stateToEntry(blendedState, m_model->geometry.fuel_kg, *playerEntry);
            m_lastPredPos = blended;
            return;
        }
    }

    // Hard snap (default path, or divergence exceeds threshold).
    stateToEntry(m_integrator->state(), m_model->geometry.fuel_kg, *playerEntry);
    m_lastPredPos = newPredPos;
    m_hasPrevPrediction = true;
}

} // namespace fl
