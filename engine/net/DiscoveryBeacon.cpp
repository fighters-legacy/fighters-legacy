// SPDX-License-Identifier: GPL-3.0-or-later
#include "DiscoveryBeacon.h"
#include "net/GameProtocol.h"

#include <ILogger.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace fl {

DiscoveryBeacon::DiscoveryBeacon(const Config& cfg, ILogger& log) : m_cfg(cfg), m_log(&log) {
    // WsaGuard takes an OS-refcounted reference; WSAEALREADY (ENet got there first) is fine.
    if (!openSock4())
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryBeacon: IPv4 socket unavailable");
    if (!openSock6())
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryBeacon: IPv6 socket unavailable");
}

DiscoveryBeacon::~DiscoveryBeacon() {
    if (sockValid(m_sock4)) {
        closeSocket(m_sock4);
        m_sock4 = kInvalidSocket;
    }
    if (sockValid(m_sock6)) {
        closeSocket(m_sock6);
        m_sock6 = kInvalidSocket;
    }
}

bool DiscoveryBeacon::isOpen() const noexcept {
    return sockValid(m_sock4) || sockValid(m_sock6);
}

void DiscoveryBeacon::tick(const TickState& state) {
    if (!isOpen())
        return;
    if (m_firstTick) {
        m_firstTick = false;
        m_lastShutdownActive = state.shuttingDown;
        m_lastSend = std::chrono::steady_clock::now();
        send(state);
        return;
    }
    auto now = std::chrono::steady_clock::now();
    // Send immediately when the shutdown state flips, so a browser sees the transition promptly (#226).
    if (state.shuttingDown != m_lastShutdownActive) {
        m_lastShutdownActive = state.shuttingDown;
        m_lastSend = now;
        send(state);
        return;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSend).count();
    if (elapsed >= static_cast<long long>(m_cfg.intervalMs)) {
        m_lastSend = now;
        send(state);
    }
}

bool DiscoveryBeacon::openSock4() {
    m_sock4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (!sockValid(m_sock4))
        return false;
    int opt = 1;
    if (setsockopt(m_sock4, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
        closeSocket(m_sock4);
        m_sock4 = kInvalidSocket;
        return false;
    }
    return true;
}

bool DiscoveryBeacon::openSock6() {
    m_sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (!sockValid(m_sock6))
        return false;
    int hops = 1;
    if (setsockopt(m_sock6, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, reinterpret_cast<const char*>(&hops), sizeof(hops)) !=
        0) {
        closeSocket(m_sock6);
        m_sock6 = kInvalidSocket;
        return false;
    }
    // "Any interface": a DWORD on Windows, an unsigned int on POSIX. Same zero, same bytes, but the
    // declared type has to match what setsockopt reads on each platform.
#if defined(_WIN32)
    DWORD ifIdx = 0;
#else
    unsigned ifIdx = 0;
#endif
    setsockopt(m_sock6, IPPROTO_IPV6, IPV6_MULTICAST_IF, reinterpret_cast<const char*>(&ifIdx), sizeof(ifIdx));
    return true;
}

void DiscoveryBeacon::send(const TickState& state) {
    fl::MsgLanBeacon pkt;
    pkt.protocolVersion = fl::kProtocolVersion;
    pkt.gamePort = m_cfg.gamePort; // where to CONNECT — independent of where this beacon is sent
    pkt.playerCount = static_cast<uint8_t>(std::clamp(state.playerCount, 0, 255));
    pkt.maxPlayers = m_cfg.maxPlayers;
    pkt.gameModeFlags = m_cfg.gameModeFlags;
    pkt.queryPort = m_cfg.queryPort; // #997
    if (state.shuttingDown) {
        pkt.gameModeFlags |= fl::kGameModeShuttingDown; // #226
        pkt.shutdownSeconds = state.shutdownSeconds;
    }
    std::snprintf(pkt.name, sizeof(pkt.name), "%s", m_cfg.name.c_str());
    std::snprintf(pkt.build, sizeof(pkt.build), "%s", m_cfg.buildVersion.c_str()); // #1074

    uint8_t buf[sizeof(fl::MsgLanBeacon)];
    std::memcpy(buf, &pkt, sizeof(pkt));

    // IPv4 broadcast
    if (sockValid(m_sock4)) {
        sockaddr_in d4{};
        d4.sin_family = AF_INET;
        d4.sin_port = htons(m_cfg.discoveryPort);
        inet_pton(AF_INET, m_cfg.broadcastAddr.c_str(), &d4.sin_addr);
        if (sendto(m_sock4, reinterpret_cast<const char*>(buf), static_cast<int>(sizeof(buf)), 0,
                   reinterpret_cast<const sockaddr*>(&d4), static_cast<int>(sizeof(d4))) < 0)
            m_log->log(LogLevel::Warn, __FILE__, __LINE__, "DiscoveryBeacon: IPv4 sendto failed");
    }

    // IPv6 link-local multicast — ff02::1 (all-nodes on link, no join needed for sender)
    if (sockValid(m_sock6)) {
        sockaddr_in6 d6{};
        d6.sin6_family = AF_INET6;
        d6.sin6_port = htons(m_cfg.discoveryPort);
        d6.sin6_scope_id = 0;
        inet_pton(AF_INET6, "ff02::1", &d6.sin6_addr);
        if (sendto(m_sock6, reinterpret_cast<const char*>(buf), static_cast<int>(sizeof(buf)), 0,
                   reinterpret_cast<const sockaddr*>(&d6), static_cast<int>(sizeof(d6))) < 0)
            m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryBeacon: IPv6 sendto failed (no IPv6 link?)");
    }
}

} // namespace fl
