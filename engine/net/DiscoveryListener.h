// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h> // inet_pton / inet_ntop on Windows
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "net/GameProtocol.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class ILogger;

// Listens on a raw UDP socket for MsgLanBeacon packets from fl-server instances on the LAN.
// Maintains a server list with TTL-based pruning. Independent of ENet.
//
// Threading: all methods must be called from the main thread.
class DiscoveryListener {
  public:
    struct ServerInfo {
        std::string address;     // source IP (IPv4 preferred over IPv6 link-local)
        fl::MsgLanBeacon beacon; // full parsed wire struct
        std::chrono::steady_clock::time_point lastSeen;
    };

    // port:  UDP port to listen on (must match DiscoveryBeacon::Config::port).
    // log:   logger reference — must outlive this object.
    // ttlMs: milliseconds before a server entry is pruned if no beacon is received.
    explicit DiscoveryListener(uint16_t port, ILogger& log, int ttlMs = 10000);
    ~DiscoveryListener();

    DiscoveryListener(const DiscoveryListener&) = delete;
    DiscoveryListener& operator=(const DiscoveryListener&) = delete;

    // Non-blocking poll: drains all pending datagrams, upserts server list, prunes stale entries.
    // Call once per frame from the main thread.
    void poll();

    // Returns a snapshot of the current server list. Small list — copy is cheap.
    std::vector<ServerInfo> servers() const;

    // Returns true if at least one socket (IPv4 or IPv6) opened successfully.
    bool isOpen() const noexcept;

  private:
#if defined(_WIN32)
    void drainSock(SOCKET sock, bool isIPv6);
#else
    void drainSock(int sock, bool isIPv6);
#endif
    bool openSock4(uint16_t port);
    bool openSock6(uint16_t port);

#if defined(_WIN32)
    SOCKET m_sock4{INVALID_SOCKET};
    SOCKET m_sock6{INVALID_SOCKET};
    bool m_wsaOwner{false};
#else
    int m_sock4{-1};
    int m_sock6{-1};
#endif

    int m_ttlMs;
    ILogger* m_log{nullptr};
    std::vector<ServerInfo> m_servers;
};
