// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "INetwork.h"
#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

struct _ENetHost; // typedef'd as ENetHost in enet/enet.h
struct _ENetPeer; // typedef'd as ENetPeer in enet/enet.h

class ENetNetwork : public INetwork {
  public:
    ENetNetwork() = default;
    ~ENetNetwork() override; // RAII: calls shutdown() if still initialized

    // Non-copyable, non-movable — owns m_host pointer
    ENetNetwork(const ENetNetwork&) = delete;
    ENetNetwork& operator=(const ENetNetwork&) = delete;
    ENetNetwork(ENetNetwork&&) = delete;
    ENetNetwork& operator=(ENetNetwork&&) = delete;

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

    // Set aggregate host bandwidth caps (bytes/s). Call once after bind().
    // 0 = unlimited (ENet default). Not part of INetwork — server-only, called at startup.
    void setBandwidthLimit(uint32_t incomingBps, uint32_t outgoingBps);

    // Pre-handshake rate limiting — drop CONNECT packets from IPs that exceed
    // maxAttempts within windowMs milliseconds before ENet peer state is allocated.
    // maxAttempts = 0 disables the filter. Not part of INetwork — server-only.
    void setPreHandshakeRateLimit(int maxAttempts, int windowMs);
    void setPreHandshakeClockOverride(std::function<std::chrono::steady_clock::time_point()> fn);

    // Called from the ENet intercept callback (ENetNetwork.cpp anonymous namespace).
    // Returns true = allow, false = drop.
    bool checkPreHandshakeConnect(const char* ip) noexcept;

  private:
    void drainPeers(); // graceful disconnect + 100 ms drain; used by disconnect() and shutdown()

    static constexpr unsigned char kChReliable = 0;
    static constexpr unsigned char kChUnreliable = 1;
    static constexpr int kChannelCount = 2;

    _ENetHost* m_host{nullptr};
    INetworkEventHandler* m_handler{nullptr};
    bool m_isServer{false};
    bool m_initialized{false};
    mutable std::string m_lastError;
    mutable std::string m_peerAddressBuf; // backing store for getPeerAddress()

    struct PreHandshakeRecord {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };
    std::unordered_map<std::string, PreHandshakeRecord> m_preHandshakeRecords;
    int m_preHandshakeRateLimit{20}; // 0 = disabled
    int m_preHandshakeWindowMs{1000};
    std::function<std::chrono::steady_clock::time_point()> m_preHandshakeNow{std::chrono::steady_clock::now};
};
