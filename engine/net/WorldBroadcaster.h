// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "INetwork.h"
#include "entity/EntityId.h"
#include "loop/ISimUpdate.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

class ILogger;

namespace fl {
class EntityManager;
class FlightIntegrator; // full definition in WorldBroadcaster.cpp
struct EntityState;
class EntityTypeRegistry;
class WeatherController;
} // namespace fl

namespace fl {

// Parsed, validated client input stored per connected peer.
struct PeerInputState {
    float throttle{0.f};
    float elevator{0.f};
    float aileron{0.f};
    float rudder{0.f};
    float viewAxis[3]{1.f, 0.f, 0.f};
    uint8_t buttons{0};
};

// Wraps EntityManager to provide a server-side ISimUpdate that:
//   1. Advances each peer's FlightIntegrator from stored client inputs.
//   2. Advances the entity simulation each tick (calls EntityManager::onTick).
//   3. Serializes live entity state into a MsgWorldSnapshot packet.
//   4. Broadcasts the packet to all connected clients via INetwork.
//   5. Calls INetwork::service(0) to flush the outbound ENet queue.
//
// Also implements INetworkEventHandler to:
//   - Spawn a player entity, create its FlightIntegrator, and send MsgConnectAck on connect.
//   - Kill the player entity and tear down its FlightIntegrator on disconnect.
//   - Decode and validate MsgClientInput packets.
//
// Threading: all ISimUpdate and INetworkEventHandler methods are called from
// the GameLoop sim thread. INetwork::setEventHandler(&broadcaster) must be
// called before GameLoop::start().
class WorldBroadcaster : public ISimUpdate, public INetworkEventHandler {
  public:
    // weather may be nullptr; when non-null it is ticked and broadcast each sim tick.
    WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net, ILogger& logger,
                     WeatherController* weather = nullptr);
    ~WorldBroadcaster(); // defined in .cpp — FlightIntegrator must be complete at destruction

    // ISimUpdate
    void onTick(double simDt, uint64_t tickIndex) override;

    // INetworkEventHandler
    void onConnect(uint32_t peerId) override;
    void onDisconnect(uint32_t peerId) override;
    void onReceive(uint32_t peerId, const void* data, std::size_t size) override;

    // Safe to call from any thread (main thread reads this for the LAN discovery beacon).
    int getPeerCount() const noexcept {
        return m_activePeerCount.load(std::memory_order_relaxed);
    }

  private:
    void sendConnectAck(uint32_t peerId, EntityId assigned);
    void stepFlightSim(FlightIntegrator& fi, EntityState& state, const PeerInputState& inp, double simDt);

    EntityManager& m_entityManager;
    EntityTypeRegistry& m_registry;
    INetwork& m_net;
    ILogger& m_logger;
    WeatherController* m_weather{nullptr};

    std::unordered_map<uint32_t, EntityId> m_peerEntities;
    std::unordered_map<uint32_t, PeerInputState> m_peerInputs;
    std::unordered_map<uint32_t, std::unique_ptr<FlightIntegrator>> m_peerFlightSims;

    std::atomic<int> m_activePeerCount{0};
    uint64_t m_weatherBroadcastTick{0}; // throttle weather broadcasts to ~6 Hz
    uint32_t m_turbRng{0xCAFEBABEu};    // per-broadcaster RNG for turbulence perturbation
};

} // namespace fl
