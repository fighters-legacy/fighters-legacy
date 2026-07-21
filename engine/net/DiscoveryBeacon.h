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

#include <chrono>
#include <cstdint>
#include <string>

namespace fl {

class ILogger;

// Broadcasts a MsgLanBeacon UDP packet on both IPv4 (255.255.255.255) and IPv6 link-local
// multicast (ff02::1) at a configurable interval so clients on the same LAN can discover
// fl-server without knowing its IP address. Independent of ENet.
//
// Threading: all methods must be called from the main thread.
class DiscoveryBeacon {
  public:
    struct Config {
        std::string name;
        uint16_t port{4778};
        uint8_t maxPlayers{32};
        uint8_t gameModeFlags{0};
        int intervalMs{2000};
        std::string broadcastAddr{"255.255.255.255"}; // configurable for loopback tests
    };

    explicit DiscoveryBeacon(const Config& cfg, ILogger& log);
    ~DiscoveryBeacon();

    DiscoveryBeacon(const DiscoveryBeacon&) = delete;
    DiscoveryBeacon& operator=(const DiscoveryBeacon&) = delete;
    DiscoveryBeacon(DiscoveryBeacon&&) = delete;
    DiscoveryBeacon& operator=(DiscoveryBeacon&&) = delete;

    // Live per-tick state carried in the beacon (#226): the current player count plus whether a
    // shutdown is counting down and how many seconds remain. Passed by value each tick.
    struct TickState {
        int playerCount{0};
        bool shuttingDown{false};
        uint16_t shutdownSeconds{0};
    };

    // Sends a beacon if intervalMs has elapsed (first call fires immediately), OR immediately when the
    // shutdown state changes so browsers see the transition within one poll rather than up to intervalMs.
    void tick(const TickState& state);

    // Returns true if at least one socket (IPv4 or IPv6) opened successfully.
    bool isOpen() const noexcept;

    // Update the server name broadcast in future beacons (e.g. after reload_config).
    // Main-thread only; takes effect on the next tick().
    void setName(std::string name) {
        m_cfg.name = std::move(name);
    }

  private:
    void send(const TickState& state);
    bool openSock4();
    bool openSock6();

#if defined(_WIN32)
    SOCKET m_sock4{INVALID_SOCKET};
    SOCKET m_sock6{INVALID_SOCKET};
    bool m_wsaOwner{false};
#else
    int m_sock4{-1};
    int m_sock6{-1};
#endif

    Config m_cfg;
    ILogger* m_log{nullptr}; // stored as pointer — ref is non-rebindable
    std::chrono::steady_clock::time_point m_lastSend{};
    bool m_firstTick{true};
    bool m_lastShutdownActive{false}; // for immediate re-send on a shutdown state change (#226)
};

} // namespace fl
