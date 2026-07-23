// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config/PredictionSettings.h"
#include "flight/CentralGravityField.h" // complete type required for std::optional<CentralGravityField>
#include "flight/FlightIntegrator.h"    // complete type required for std::unique_ptr destructor
#include "net/GameProtocol.h"           // MsgClientInput
#include "net/JitterBuffer.h"           // BufferedInput
#include "render/RenderSnapshot.h"

#include <functional>
#include <memory>
#include <optional>

namespace fl {

struct EnvironmentState;
enum class SurfaceType : uint8_t; // engine/render/SurfaceType.h — ground-surface handling (#487)

// Client-side prediction for the player's own entity.
//
// Call init() once after connecting (resolver/config). Call onInput() before
// each MsgClientInput is sent — this steps the local FlightIntegrator and
// stores the input in the 128-slot history ring. Wire reconcile() as
// ClientNetEventHandler::snapshotCallback so it is called after each snapshot
// is assembled, before publishExternal(). It resets the integrator to the
// server's authoritative state, replays the stored inputs for the delay window,
// and mutates the player's EntityRenderEntry in-place.
//
// Other entities are untouched — they remain server-authoritative.
class ClientPrediction {
  public:
    // Resolver: typeIndex → FlightModelData (or BuiltinFlightModel as fallback).
    // The lambda in Game.cpp captures EntityTypeRegistry + AssetManager and does
    // the full lookup: typeIndex → entity def → flightModelAsset → parseFlightModel.
    using FlightModelResolver = std::function<std::shared_ptr<const FlightModelData>(uint32_t typeIndex)>;
    // Resolver: typeIndex → what the type's default loadout costs the airframe (#812). The two floats
    // arrive on MsgEntityTypeDef, so the client reads them off EntityDef rather than owning a weapon
    // registry it has no other use for. Unset ⇒ a clean airframe — which would silently make the
    // client lighter and slicker than the server, so Game.cpp always wires it.
    using PayloadResolver = std::function<PayloadEffect(uint32_t typeIndex)>;
    // worldPos → terrain elevation (m) above the datum along the radial (TerrainStreamer::heightAt(dvec3)).
    // FlightIntegrator compares it against the geodetic altitude for radial ground contact (#477).
    using HeightQuery = std::function<float(glm::dvec3 worldPos)>;
    // worldPos → terrain SurfaceType (TerrainStreamer::surfaceTypeAt, incl. the runway override) for
    // per-surface rolling resistance (#487). The server applies the identical groundFrictionFor table,
    // so the predicted rollout stays in parity. Optional (unset ⇒ paved default).
    using SurfaceQuery = std::function<SurfaceType(glm::dvec3 worldPos)>;

    ClientPrediction() = default;
    ~ClientPrediction();

    // Wire the ground-surface query (#487). Optional; call after init() when a terrain streamer with
    // the runway-surface override exists. Unset leaves the rollout on the paved default.
    void setSurfaceQuery(SurfaceQuery q) {
        m_surfaceQuery = std::move(q);
    }

    // Must be called before the first reconcile(). Resolver is invoked lazily on
    // first snapshot that contains the player's entry.
    // planetRadiusKm: from MsgConnectAck; used to match the server's gravity field.
    void init(PredictionSettings cfg, FlightModelResolver resolver, PayloadResolver payloadResolver,
              HeightQuery heightQuery, uint32_t playerIdx, uint32_t playerGen, float planetRadiusKm = 6371.f);

    // Called before each MsgClientInput is sent. Pushes input into the history
    // ring and steps the local integrator one tick (if initialized).
    void onInput(const MsgClientInput& msg, const EnvironmentState& env);

    // Sentinel for reconcile()'s ackedSeqNum when the server did not report one (mirrors
    // ClientNetEventHandler::kNoAckedSeqNum) — replay then falls back to estimatedDelayTicks.
    static constexpr uint32_t kNoAckedSeqNum = 0xFFFFFFFFu;

    // Called from ClientNetEventHandler::snapshotCallback after snapshot assembly,
    // before publishExternal(). Mutates the player's EntityRenderEntry with the
    // predicted state. No-op until init() + first snapshot with the player's entry.
    // ackedSeqNum (#427): the exact seqNum the server last applied for this peer — replay the inputs
    // NEWER than it. kNoAckedSeqNum → approximate the replay depth from estimatedDelayTicks instead.
    void reconcile(RenderSnapshot& snap, uint64_t tickIndex, uint32_t estimatedDelayTicks, uint32_t ackedSeqNum,
                   const EnvironmentState& env);

    // Clear all prediction state (session end / disconnect). Safe to call multiple times.
    void reset();

    // Hot-reload seam (#152): drop the resolved model + integrator (and its custom gravity) but KEEP
    // the input-history ring, resolver, config, and player idx/gen. The next reconcile() lazily
    // re-resolves the model from the (now cache-invalidated) resolver and re-seeds from the
    // authoritative snapshot — the existing first-snapshot init path. Call when a FlightModel asset
    // the player's aircraft uses changed.
    void invalidateModel();

    [[nodiscard]] bool isInitialized() const noexcept {
        return m_initialized;
    }

    // Read-only view of the predicted own-ship FlightState, or nullptr until init() + the first
    // snapshot with the player's entry has seeded the local integrator. Own-ship HUD/audio cues
    // (e.g. the stall/overspeed warning tones, #957) read stall/load-factor/velocity from here — the
    // honest local prediction, since the snapshot does not carry stalled/Mach.
    [[nodiscard]] const FlightState* predictedState() const noexcept {
        return (m_initialized && m_integrator) ? &m_integrator->state() : nullptr;
    }

    // The own model's never-exceed Mach (limits.max_mach), or 0 when no model is resolved yet.
    [[nodiscard]] float predictedMaxMach() const noexcept {
        return m_model ? m_model->limits.max_mach : 0.f;
    }

  private:
    static constexpr uint32_t kHistorySize = 128u;

    struct HistoryEntry {
        uint32_t seqNum{0};
        BufferedInput input{};
    };

    void pushHistory(uint32_t seqNum, const BufferedInput& bi) noexcept;
    // Fills out[0..count-1] with the last `count` history entries, oldest first.
    // Returns the actual number written (may be less than count if history is shallow).
    uint32_t tailHistory(uint32_t count, HistoryEntry* out) const noexcept;

    void stepIntegrator(const BufferedInput& bi, const EnvironmentState& env);

    PredictionSettings m_cfg{};
    FlightModelResolver m_resolver;
    PayloadResolver m_payloadResolver;
    HeightQuery m_heightQuery;
    SurfaceQuery m_surfaceQuery; // #487 per-surface rolling resistance (paved default when unset)
    uint32_t m_playerIdx{0};
    uint32_t m_playerGen{0};
    float m_planetRadiusKm{6371.f};

    // The server tick this integrator's state corresponds to. Set from the snapshot in reconcile()
    // and advanced by one on every stepIntegrator() call, so a replayed input is seeded with the same
    // tick the server used for it. It is the seed for the deterministic stall buffet (#816): if the
    // delay estimate is off by a tick the buffet differs slightly, which reconciliation absorbs like
    // any other prediction error -- unlike weather turbulence, which could never be reproduced at all.
    uint64_t m_predictedTick{0};

    bool m_initialized{false};
    std::shared_ptr<const FlightModelData> m_model;
    PayloadEffect m_payload{}; // resolved with m_model on the first snapshot; the server does the same at spawn
    std::unique_ptr<FlightIntegrator> m_integrator;
    // Stored when planetRadiusKm differs from Earth; setGravityField() holds a ref to it.
    std::optional<CentralGravityField> m_customGravity;

    // Input history ring — plain C array to avoid std::array<T, uint32_t> MSVC warning.
    HistoryEntry m_history[kHistorySize]{};
    uint32_t m_histHead{0};  // index of oldest entry
    uint32_t m_histCount{0}; // number of valid entries [0, kHistorySize]

    // Last predicted position, for snap vs. blend decision.
    glm::dvec3 m_lastPredPos{};
    bool m_hasPrevPrediction{false};
};

} // namespace fl
