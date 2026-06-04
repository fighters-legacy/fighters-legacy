// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "INetwork.h"
#include "entity/EntityId.h"
#include "loop/ISimUpdate.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

    // Peer management — all must be called from the sim thread (via GameLoop::enqueueSimCallback).

    // Gracefully disconnect one peer by ID.
    void kickPeer(uint32_t peerId);

    // Add a normalized IP to the in-memory ban set and kick any currently connected peers
    // with that IP. ip may be plain IPv4 ("1.2.3.4"), bare IPv6 ("::1"), bracketed IPv6
    // ("[::1]"), or IPv4-mapped IPv6 ("::ffff:1.2.3.4" or "[::ffff:1.2.3.4]").
    void banAddress(std::string ip);

    // Remove an IP from the ban set (same normalization rules as banAddress).
    void unbanAddress(const std::string& ip);

    // Iterate all connected peers. fn receives (peerId, full "ip:port" address string, EntityId).
    // The address string is copied per entry — safe despite INetwork::getPeerAddress() returning
    // a pointer backed by a single overwrite buffer.
    void forEachPeer(std::function<void(uint32_t peerId, const std::string& addr, EntityId eid)> fn) const;

    // Replace the entire in-memory ban set. Safe to call before gameLoop.start().
    void setBannedAddresses(std::unordered_set<std::string> addrs);

    // Set the allowlist. Empty set = allowlist disabled (all IPs permitted).
    void setAllowedAddresses(std::unordered_set<std::string> addrs);

    // Return a copy of the current ban set (called from sim thread to save to file).
    std::unordered_set<std::string> getBannedAddresses() const;

    // Configure rate limiting; call before gameLoop.start().
    void setRateLimitParams(int maxConnects, int windowSeconds, int floodMultiplier);

    // Override the clock used for rate limiting (for testing only).
    void setClockOverride(std::function<std::chrono::steady_clock::time_point()> fn);

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

    std::unordered_set<std::string> m_bannedAddresses; // in-memory ban list; sim-thread only

    // Per-IP sliding-window connection rate limiter (sim-thread only).
    struct ConnectRecord {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };
    std::unordered_map<std::string, ConnectRecord> m_connectRecords;
    int m_connectRateLimit{5};
    int m_connectRateWindowS{10};
    uint64_t m_ratePruneTick{0}; // coarse prune cadence counter (every 600 ticks)

    // Per-peer packet flood detector (sim-thread only).
    struct PeerFloodState {
        uint32_t packetCount{0};
        std::chrono::steady_clock::time_point windowStart{};
    };
    std::unordered_map<uint32_t, PeerFloodState> m_peerFloodState;
    int m_floodMultiplier{3};

    std::unordered_set<std::string> m_allowedAddresses; // empty = allowlist disabled

    // Injectable clock for testing; defaults to steady_clock::now.
    std::function<std::chrono::steady_clock::time_point()> m_now{std::chrono::steady_clock::now};
};

} // namespace fl
