// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IClock.h"
#include "INetwork.h"
#include "WireRateSampler.h"
#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

struct _ENetHost; // typedef'd as ENetHost in enet/enet.h
struct _ENetPeer; // typedef'd as ENetPeer in enet/enet.h

namespace fl {

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
    uint32_t getPeerRtt(uint32_t peerId) const override;
    PeerLinkStats getPeerLinkStats(uint32_t peerId) const override;
    WireStats getWireStats() const override;

    // The port this host is actually bound to, or 0 if not bound.
    //
    // Exists so callers can bind an EPHEMERAL port (`bind(addr, 0, n)`, letting the OS pick a free
    // one) and then discover which one they got. That is the only way to bind a socket in a process
    // that may be running alongside arbitrary other processes without risking a collision — which is
    // exactly the situation every test is in under `ctest -j`, where each Catch2 TEST_CASE runs as
    // its own process (#787). Hardcoded test ports are a race waiting for a many-core box.
    //
    // Not on INetwork: this is a test/diagnostic affordance, and putting it on the interface would
    // oblige every mock and the GNS backend to implement it for no gain.
    [[nodiscard]] uint16_t boundPort() const;

    // Set aggregate host bandwidth caps (bytes/s). Call once after bind().
    // 0 = unlimited (ENet default). INetwork server-only tuning hook.
    void setBandwidthLimit(uint32_t incomingBps, uint32_t outgoingBps) override;

    // Pre-handshake rate limiting — drop CONNECT packets from IPs that exceed
    // maxAttempts within windowMs milliseconds before ENet peer state is allocated.
    // maxAttempts = 0 disables the filter. INetwork server-only tuning hook.
    void setPreHandshakeRateLimit(int maxAttempts, int windowMs) override;
    // Clock injection for the pre-handshake filter (tests only) — ENet-concrete, not on INetwork.
    void setPreHandshakeClock(const fl::IClock& clock);

    // Called from the ENet intercept callback (ENetNetwork.cpp anonymous namespace).
    // Returns true = allow, false = drop.
    bool checkPreHandshakeConnect(const char* ip) noexcept;

  private:
    void drainPeers(); // graceful disconnect + 100 ms drain; used by disconnect() and shutdown()

    static constexpr unsigned char kChReliable = 0;
    static constexpr unsigned char kChUnreliable = 1;
    static constexpr int kChannelCount = 2;

    _ENetHost* m_host{nullptr};
    // Cumulative-counter -> rate conversion for getWireStats (#772). Mutable because the
    // getter is const and the sampler must remember the previous sample to form a delta.
    mutable WireRateSampler m_wireSampler;
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
    const fl::IClock* m_preHandshakeClock{&fl::SystemClock::instance()};
};

} // namespace fl
