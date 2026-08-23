// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SocketCompat.h> // socket_t / WsaGuard / the winsock include order (#1256)

#include "net/GameProtocol.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

class ILogger;

// Client side of the server-info query protocol (#997). Sends MsgServerQuery to a server's query port
// and collects MsgServerInfo replies, computing RTT from a LOCAL nonce -> send-time map (it never
// trusts the echoed clientTimeMs for arithmetic). Main-thread, non-blocking poll — the DiscoveryListener
// idiom. Used by the server browser (#143) for LAN-discovered and lobby-listed servers alike.
class ServerQueryClient {
  public:
    struct Result {
        std::string address;
        uint16_t queryPort{0};
        MsgServerInfo info{};
        float rttMs{0.f};
        std::chrono::steady_clock::time_point receivedAt{};
    };

    explicit ServerQueryClient(ILogger& log, int timeoutMs = 3000);
    ~ServerQueryClient();

    ServerQueryClient(const ServerQueryClient&) = delete;
    ServerQueryClient& operator=(const ServerQueryClient&) = delete;

    // Send a query to host:queryPort. Returns the nonce used (0 = send failed). Records the send time.
    uint32_t query(const std::string& host, uint16_t queryPort);

    // Drain any pending replies, match them to outstanding nonces, and compute RTT. Non-blocking.
    void poll();

    // Latest result per (address, queryPort). Sorted is the caller's concern.
    [[nodiscard]] std::vector<Result> results() const;

    void clear();

    [[nodiscard]] bool isOpen() const noexcept;

  private:
    struct Pending {
        std::string address;
        uint16_t queryPort{0};
        std::chrono::steady_clock::time_point sentAt{};
    };

    WsaGuard m_wsa; // #1256: OS-refcounted, so taking a reference is free
    socket_t m_sock{kInvalidSocket};
    ILogger* m_log{nullptr};
    int m_timeoutMs{3000};
    uint32_t m_nextNonce{1};
    std::unordered_map<uint32_t, Pending> m_pending;
    std::unordered_map<std::string, Result> m_results; // keyed "address:port"
};

} // namespace fl
