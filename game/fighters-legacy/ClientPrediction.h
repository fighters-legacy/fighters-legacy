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
    // the full lookup: typeIndex → entity def → flightModelId → parseFlightModel.
    using FlightModelResolver = std::function<std::shared_ptr<const FlightModelData>(uint32_t typeIndex)>;
    using HeightQuery = std::function<float(double x, double z)>;

    ClientPrediction() = default;
    ~ClientPrediction();

    // Must be called before the first reconcile(). Resolver is invoked lazily on
    // first snapshot that contains the player's entry.
    // planetRadiusKm: from MsgConnectAck; used to match the server's gravity field.
    void init(PredictionSettings cfg, FlightModelResolver resolver, HeightQuery heightQuery, uint32_t playerIdx,
              uint32_t playerGen, float planetRadiusKm = 6371.f);

    // Called before each MsgClientInput is sent. Pushes input into the history
    // ring and steps the local integrator one tick (if initialized).
    void onInput(const MsgClientInput& msg, const EnvironmentState& env);

    // Called from ClientNetEventHandler::snapshotCallback after snapshot assembly,
    // before publishExternal(). Mutates the player's EntityRenderEntry with the
    // predicted state. No-op until init() + first snapshot with the player's entry.
    void reconcile(RenderSnapshot& snap, uint64_t tickIndex, uint32_t estimatedDelayTicks, const EnvironmentState& env);

    // Clear all prediction state (session end / disconnect). Safe to call multiple times.
    void reset();

    [[nodiscard]] bool isInitialized() const noexcept {
        return m_initialized;
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
    HeightQuery m_heightQuery;
    uint32_t m_playerIdx{0};
    uint32_t m_playerGen{0};
    float m_planetRadiusKm{6371.f};

    bool m_initialized{false};
    std::shared_ptr<const FlightModelData> m_model;
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
