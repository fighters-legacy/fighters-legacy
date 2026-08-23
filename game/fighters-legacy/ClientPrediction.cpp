// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientPrediction.h"

#include "RenderTypes.h"       // EnvironmentState
#include "flight/AeroForces.h" // ControlInput
#include "flight/ArticulationChannels.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelData.h"
#include "flight/ForceModelSelect.h" // applyForceModelFor — the shared role → force-model seam (#349)
#include "flight/Geodetic.h"         // kEarthRotationRate (#482)
#include "flight/StallBuffet.h"
#include "render/SurfaceType.h" // groundFrictionFor (#487)
#include "weather/Turbulence.h"
#include "weather/WindProfile.h" // altitude wind interp (#489)

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace fl {

namespace {
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

// Overwrite an EntityRenderEntry from a predicted FlightState. The model supplies the tank capacity
// (fuel_kg goes back out as 0-100 percent) and, since #1195, the wing-sweep limits the `sweep`
// articulation channel is normalized against.
static void stateToEntry(const FlightState& fs, const FlightModelData& model, EntityRenderEntry& out) {
    const float maxFuel = model.geometry.fuel_kg;
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

    // Own-ship articulation (#843): the locally integrated, FULL-PRECISION positions overwrite
    // whatever the cosmetic snapshot TLV carried for us. That is the reason the TLV can be quantized
    // to 1/255 at all — the authoritative path for a peer's own aircraft never reads the wire.
    // Built through the SAME fillArtChannels the server sends from (#1195), so the player's own
    // aircraft can never again render a channel the rest of the world is being told about.
    fillArtChannels(fs, model, out.artChannels);
}
} // namespace

ClientPrediction::~ClientPrediction() = default;

void ClientPrediction::init(PredictionSettings cfg, FlightModelResolver resolver, PayloadResolver payloadResolver,
                            HeightQuery heightQuery, uint32_t playerIdx, uint32_t playerGen, float planetRadiusKm,
                            TickRate serverTickRate) {
    reset();
    m_cfg = cfg;
    m_resolver = std::move(resolver);
    m_payloadResolver = std::move(payloadResolver);
    m_heightQuery = std::move(heightQuery);
    m_playerIdx = playerIdx;
    m_playerGen = playerGen;
    m_planetRadiusKm = planetRadiusKm;
    m_serverTickRate = serverTickRate;
}

void ClientPrediction::invalidateModel() {
    // Keep the history ring / resolver / idx-gen; only drop the resolved model so the next reconcile()
    // re-resolves and re-seeds via the existing first-snapshot path (#152).
    m_initialized = false;
    m_integrator.reset();
    m_model.reset();
    m_customGravity.reset();
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
    // Snapshot the actuator positions BEFORE this input is applied (#843), so a replay can rewind to
    // exactly where they were at the acked point rather than re-integrating from a reset zero.
    const ArticulationState artBefore = m_integrator ? m_integrator->state().articulation : ArticulationState{};
    const float sweepBefore = m_integrator ? m_integrator->state().current_sweep_deg : 0.f;
    m_history[writeIdx] = {seqNum, bi, artBefore, sweepBefore};
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
    ++m_predictedTick; // the server tick this input will be (or was) applied on
    if (!m_integrator) {
        return;
    }
    ControlInput ctrl;
    ctrl.throttle = bi.throttle;
    ctrl.elevator = bi.elevator;
    ctrl.aileron = bi.aileron;
    ctrl.rudder = bi.rudder;
    ctrl.afterburner = (bi.buttons & 0x02u) != 0;
    // Articulation commands (#843): the client integrates the same actuator transit the server does,
    // through the same pure advanceArticulation, so gear/flap DRAG matches tick for tick.
    ctrl.flaps = static_cast<float>(bi.flaps) / 255.f;
    ctrl.speedbrake = static_cast<float>(bi.speedbrake) / 255.f;
    ctrl.gear_down = (bi.artButtons & kArtButtonGearDown) != 0;
    ctrl.hook_down = (bi.artButtons & kArtButtonHookDown) != 0;
    ctrl.canopy_open = (bi.artButtons & kArtButtonCanopyOpen) != 0;

    // Altitude wind (#489): interpolate the broadcast profile at the predicted entity's altitude with
    // the SAME shared code the server runs, so a high-flying aircraft is predicted in the wind it
    // actually flies in. Falls back to the datum scalar when no profile is present (byte-identical).
    const auto& predPos = m_integrator->state().pos_world;
    const float altM = static_cast<float>(
        windAltitudeM(glm::dvec3(predPos[0], predPos[1], predPos[2]), static_cast<double>(m_planetRadiusKm) * 1000.0));
    const glm::vec2 wprof = windAtAltitude(env, altM);
    WindInfluence wind;
    wind.wind_world[0] = wprof.x;
    wind.wind_world[1] = 0.f;
    wind.wind_world[2] = wprof.y;

    // Weather turbulence (#426): reproduced EXACTLY from the amplitude the server broadcasts in
    // MsgWeatherState. The server's turbulence was always a deterministic function of
    // (entityIdx, tickIndex) + amplitude, so once the amplitude reaches the client the SAME shared
    // weatherTurbulence() yields the identical perturbation — no shared-PRNG state needed. Before
    // this the client predicted zero turbulence and diverged by the full amplitude every gusty tick.
    const auto turb = weatherTurbulence(m_playerIdx, m_predictedTick, env.turbulenceAmp);
    wind.turbulence_body[0] = turb[0];
    wind.turbulence_body[1] = turb[1];
    wind.turbulence_body[2] = turb[2];

    // Stall buffet (#816). The server folds the SAME deterministic (entityIdx, tickIndex) buffet into
    // its turbulence, so unlike weather turbulence this one CAN be predicted -- and must be, or the
    // aircraft would diverge from the server exactly when it is stalled. See StallBuffet.h.
    if (m_integrator->state().stalled) {
        const auto buffet = stallBuffet(m_playerIdx, m_predictedTick);
        wind.turbulence_body[0] += buffet[0];
        wind.turbulence_body[1] += buffet[1];
        wind.turbulence_body[2] += buffet[2];
    }

    const glm::dvec3 groundPos{m_integrator->state().pos_world[0], m_integrator->state().pos_world[1],
                               m_integrator->state().pos_world[2]};
    const float groundElev = m_heightQuery ? m_heightQuery(groundPos) : 0.f;
    // Per-surface rolling resistance (#487), the same groundFrictionFor table the server applies, so
    // the predicted rollout matches. Paved default when no surface query is wired.
    const fl::GroundFriction ground =
        m_surfaceQuery ? groundFrictionFor(m_surfaceQuery(groundPos)) : fl::GroundFriction{};
    m_integrator->step(m_serverTickRate.dtSeconds(), ctrl, m_payload, wind, groundElev, ground);
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
    bi.flaps = msg.flaps;           // #843 articulation commands, absolute state
    bi.speedbrake = msg.speedbrake; // #843
    bi.artButtons = msg.artButtons; // #843

    pushHistory(msg.seqNum, bi);

    if (m_initialized) {
        stepIntegrator(bi, env);
    }
}

void ClientPrediction::reconcile(RenderSnapshot& snap, uint64_t tickIndex, uint32_t estimatedDelayTicks,
                                 uint32_t ackedSeqNum, const EnvironmentState& env) {
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
        // The stores the server is flying with. Resolved here, beside the model, because the server
        // resolves its payload once at spawn too -- if the two disagree, prediction diverges by the
        // exact weight and drag of the loadout, which is precisely the bug #812 exists to prevent.
        m_payload = m_payloadResolver ? m_payloadResolver(playerEntry->typeIndex) : PayloadEffect{};
        m_integrator = std::make_unique<FlightIntegrator>(m_model);
        // Bind the same force model the server bound at spawn (#349). Until this line existed the
        // client always replayed FixedWingForceModel — survivable while the only non-fixed-wing
        // role was AI-flown; a player-predicted rotorcraft would diverge every tick without it.
        applyForceModelFor(*m_integrator, *m_model);
        // Match the server's gravity field for non-Earth planet radii.
        if (std::abs(m_planetRadiusKm - 6371.f) > 1.f) {
            m_customGravity.emplace(m_planetRadiusKm * 1000.f);
            m_integrator->setGravityField(*m_customGravity);
        }
        // Match the server's Earth-fixed rotating frame (#482). The Coriolis/centrifugal terms are
        // deterministic (pos/vel only, no RNG), so predicting with the same rate the default server
        // uses keeps reconciliation exact; a server that disabled earth_rotation leaves only a tiny
        // persistent bias the blend absorbs.
        m_integrator->setEarthRotationRate(kEarthRotationRate);
        m_initialized = true;
    }

    // Reset integrator to the server's authoritative state. The state we are resetting to IS the
    // state at `tickIndex`, so the replayed inputs below are ticks tickIndex+1, +2, ... -- which is
    // what lets the stall buffet (#816) be seeded with the same tick the server used.
    // Live payload (#625): once a store can leave the rails, the per-type default resolved at init
    // stops being the truth. The own-entity record carries what the remaining stores cost NOW; a
    // release therefore changes the client-predicted physics on the same snapshot that reports it.
    if (playerEntry->hasLoadout) {
        m_payload.extra_mass_kg = playerEntry->payloadMassKg;
        m_payload.extra_cd0 = playerEntry->payloadCd0;
    }

    FlightState serverState = entryToFlightState(*playerEntry, *m_model);

    // Rewind the actuator positions to where they stood before the FIRST input the server has not yet
    // applied (#843), then let the replay re-integrate them. Articulation is not on the own entity's
    // wire record — a peer never reads its own channels from the network, so the cosmetic 1/255 TLV
    // quantization cannot reach the authoritative path — which means the replay has to supply this
    // baseline itself. Resetting to zero here would restart the transit every reconcile, and gear
    // position IS drag: the two sides would then disagree about where the aircraft is.
    HistoryEntry replayBuf[kHistorySize];
    uint32_t replayStart = 0;
    uint32_t got = 0;
    if (ackedSeqNum != kNoAckedSeqNum) {
        // Exact window (#427): the server told us the last seqNum it applied, so replay precisely the
        // stored inputs newer than it — no over- or under-replay from a delay estimate under jitter.
        // seqNum is client-monotonic and cannot wrap within a session, so a plain > compare is safe.
        got = tailHistory(m_histCount, replayBuf);
        while (replayStart < got && replayBuf[replayStart].seqNum <= ackedSeqNum)
            ++replayStart;
    } else {
        // Fallback: approximate the replay depth from estimatedDelayTicks (the pre-#427 behaviour,
        // used until the server has applied one of our inputs, or against an older server).
        got = tailHistory(std::min(estimatedDelayTicks, m_histCount), replayBuf);
    }
    serverState.articulation =
        (replayStart < got) ? replayBuf[replayStart].artBefore : m_integrator->state().articulation;
    // Wing sweep rewinds with it (#1195). entryToFlightState seeds ref_sweep_deg because the wire
    // record carries no sweep and a first reconcile has nothing better; from the second onward the
    // integrator does, and taking the reference instead threw away every degree the schedule had
    // slewed. On a first reconcile this reads back the value the FlightIntegrator constructor just
    // set, which is the same reference — so the seed path is unchanged.
    serverState.current_sweep_deg =
        (replayStart < got) ? replayBuf[replayStart].sweepBefore : m_integrator->state().current_sweep_deg;

    m_integrator->reset(serverState);
    m_predictedTick = tickIndex;

    for (uint32_t i = replayStart; i < got; ++i)
        stepIntegrator(replayBuf[i].input, env);

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
            stateToEntry(blendedState, *m_model, *playerEntry);
            m_lastPredPos = blended;
            return;
        }
    }

    // Hard snap (default path, or divergence exceeds threshold).
    stateToEntry(m_integrator->state(), *m_model, *playerEntry);
    m_lastPredPos = newPredPos;
    m_hasPrevPrediction = true;
}

} // namespace fl
