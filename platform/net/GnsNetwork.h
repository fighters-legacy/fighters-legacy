// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "INetwork.h"
#include <cstdint>
#include <string>
#include <unordered_map>

// Forward declaration keeps GameNetworkingSockets / protobuf headers off this header's include
// path. GNS connection/socket/poll-group handles are all uint32 typedefs, so we store them as
// uint32_t here and only touch the concrete GNS types inside GnsNetwork.cpp.
class ISteamNetworkingSockets;
struct SteamNetConnectionStatusChangedCallback_t;

namespace fl {

// GameNetworkingSockets transport backend (#507). Encrypted UDP (curve25519 + AES-GCM), mature
// congestion control, 128+ connection headroom. One instance owns its own listen socket + poll
// group + peer maps over the process-global GNS interface; the global init is refcounted so many
// instances coexist (mirrors ENetNetwork's g_enetRefCount). Threading: single-thread service();
// the server host is sim-thread-owned, the client host main-thread-owned — never crossed.
class GnsNetwork : public INetwork {
  public:
    GnsNetwork() = default;
    ~GnsNetwork() override;

    GnsNetwork(const GnsNetwork&) = delete;
    GnsNetwork& operator=(const GnsNetwork&) = delete;
    GnsNetwork(GnsNetwork&&) = delete;
    GnsNetwork& operator=(GnsNetwork&&) = delete;

    bool init() override;
    void shutdown() override;
    void setEventHandler(INetworkEventHandler* handler) override;
    bool bind(const char* address, uint16_t port, int maxClients) override;
    bool connect(const char* host, uint16_t port) override;
    void disconnect() override;
    bool send(uint32_t peerId, const void* data, std::size_t size, bool reliable) override;
    void broadcast(const void* data, std::size_t size, bool reliable) override;
    void service(int timeoutMs = 0) override;
    int getPeerCount() const override;
    PeerState getPeerState(uint32_t peerId) const override;
    const char* getPeerAddress(uint32_t peerId) const override;
    void disconnectPeer(uint32_t peerId) override;
    const char* getLastError() const override;
    uint32_t getPeerRtt(uint32_t peerId) const override;
    PeerLinkStats getPeerLinkStats(uint32_t peerId) const override;
    void setBandwidthLimit(uint32_t incomingBps, uint32_t outgoingBps) override;
    // Pre-handshake flooding is handled by GNS itself; this is a documented no-op for parity.
    void setPreHandshakeRateLimit(int maxAttempts, int windowMs) override;

    // Whether unauthenticated peers are accepted (standalone GNS has no Steam PKI). Default true;
    // fl-server maps [network] allow_insecure here. Call before bind()/connect().
    void setAllowInsecure(bool allow) override {
        m_allowInsecure = allow;
    }

    // Routed here by the static connection-status trampoline (GnsNetwork.cpp). Public so the
    // trampoline can dispatch; not part of INetwork.
    void onConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t* info);

  private:
    uint32_t connForPeer(uint32_t peerId) const; // 0 (k_HSteamNetConnection_Invalid) if unmapped
    void closeAndErase(uint32_t conn, bool notify);

    ISteamNetworkingSockets* m_sockets{nullptr};
    INetworkEventHandler* m_handler{nullptr};
    uint32_t m_listenSocket{0}; // HSteamListenSocket (0 = invalid); server only
    uint32_t m_pollGroup{0};    // HSteamNetPollGroup (0 = invalid); server only
    uint32_t m_clientConn{0};   // HSteamNetConnection (0 = invalid); client only
    int m_maxClients{0};
    bool m_isServer{false};
    bool m_initialized{false};
    bool m_allowInsecure{true};

    // Bidirectional peerId <-> HSteamNetConnection maps. peerId is a small stable uint32 the rest
    // of the engine assumes (spawn maps, ack windows); the connection handle is GNS-assigned.
    std::unordered_map<uint32_t, uint32_t> m_peerToConn;
    std::unordered_map<uint32_t, uint32_t> m_connToPeer;
    uint32_t m_nextPeerId{0};

    mutable std::string m_lastError;
    mutable std::string m_peerAddressBuf;
};

} // namespace fl
