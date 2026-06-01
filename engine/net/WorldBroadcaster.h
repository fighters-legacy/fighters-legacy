// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "INetwork.h"
#include "entity/EntityId.h"
#include "loop/ISimUpdate.h"

#include <cstdint>
#include <unordered_map>

class ILogger;

namespace fl {
class EntityManager;
struct EntityState;
class EntityTypeRegistry;
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
//   1. Applies per-peer client inputs to owned entities (simple kinematics).
//   2. Advances the entity simulation each tick (calls EntityManager::onTick).
//   3. Serializes live entity state into a MsgWorldSnapshot packet.
//   4. Broadcasts the packet to all connected clients via INetwork.
//   5. Calls INetwork::service(0) to flush the outbound ENet queue.
//
// Also implements INetworkEventHandler to:
//   - Spawn a player entity and send MsgConnectAck on each new connection.
//   - Kill the player entity and clean up on disconnect.
//   - Decode and validate MsgClientInput packets.
//
// Threading: all ISimUpdate and INetworkEventHandler methods are called from
// the GameLoop sim thread. INetwork::setEventHandler(&broadcaster) must be
// called before GameLoop::start().
class WorldBroadcaster : public ISimUpdate, public INetworkEventHandler {
  public:
    WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net, ILogger& logger);

    // ISimUpdate
    void onTick(double simDt, uint64_t tickIndex) override;

    // INetworkEventHandler
    void onConnect(uint32_t peerId) override;
    void onDisconnect(uint32_t peerId) override;
    void onReceive(uint32_t peerId, const void* data, std::size_t size) override;

  private:
    void sendConnectAck(uint32_t peerId, EntityId assigned);
    void applyPeerInput(EntityState& state, const PeerInputState& inp, double simDt);

    EntityManager& m_entityManager;
    EntityTypeRegistry& m_registry;
    INetwork& m_net;
    ILogger& m_logger;

    std::unordered_map<uint32_t, EntityId> m_peerEntities;
    std::unordered_map<uint32_t, PeerInputState> m_peerInputs;
};

} // namespace fl
