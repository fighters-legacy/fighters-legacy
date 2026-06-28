// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BotClient — one synthetic game client: owns a single ENetNetwork (one ENetHost / UDP
// socket), replicates the minimum handshake (MsgHello version check -> MsgConnectAck), sends
// MsgClientInput at the configured rate, and accounts received snapshot bytes + tick progression.
// Network-coupled (not unit-tested directly); the pure logic it drives lives in the other
// bot_swarm headers. One BotClient is touched by exactly one worker thread (ENet hosts are not
// thread-safe).

#include "IFlightPattern.h"
#include "SwarmMetrics.h"
#include <INetwork.h>
#include <cstdint>
#include <memory>
#include <string>

namespace fl {

class BotClient : public INetworkEventHandler {
  public:
    BotClient(uint32_t index, const std::string& patternName, int rateHz);
    ~BotClient() override;

    BotClient(const BotClient&) = delete;
    BotClient& operator=(const BotClient&) = delete;

    // Lifecycle (all called from the owning worker thread only).
    bool connect(double now, const char* host, uint16_t port);
    void setNow(double now) {
        m_now = now;
    }
    void service();                  // non-blocking ENet pump
    void sendInputIfDue(double now); // emit MsgClientInput when the rate interval elapses
    void sampleRtt();                // refresh the latest ENet RTT estimate
    void beginDisconnect();          // queue a graceful disconnect (still needs service())
    void shutdown();                 // tear down the host

    bool connected() const {
        return m_metrics.connected;
    }
    const ClientMetrics& metrics() const {
        return m_metrics;
    }

    // INetworkEventHandler
    void onConnect(uint32_t peerId) override;
    void onDisconnect(uint32_t peerId) override;
    void onReceive(uint32_t peerId, const void* data, std::size_t size) override;

  private:
    uint32_t m_index;
    int m_rateHz;
    std::unique_ptr<IFlightPattern> m_pattern;
    std::unique_ptr<INetwork> m_net;
    ClientMetrics m_metrics;

    double m_now{0.0};          // current steady seconds, set each loop iteration
    double m_connectStart{0.0}; // when connect() was issued
    double m_activeStart{0.0};  // when onConnect fired (pattern time origin)
    double m_lastInputAt{-1.0};
    uint64_t m_lastTick{0}; // last snapshot tickIndex (echoed in MsgClientInput)
    uint32_t m_seq{0};
    bool m_versionOk{true};
};

} // namespace fl
