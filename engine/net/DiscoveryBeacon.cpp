// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "DiscoveryBeacon.h"
#include "net/GameProtocol.h"

#include <ILogger.h>
#include <algorithm>
#include <chrono>
#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fl {

DiscoveryBeacon::DiscoveryBeacon(const Config& cfg, ILogger& log) : m_cfg(cfg), m_log(&log) {
#if defined(_WIN32)
    WSADATA wsa{};
    int err = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (err == 0)
        m_wsaOwner = true;
    // WSAEALREADY means another component (e.g. enet_initialize) already started Winsock — safe.
#endif
    if (!openSock4())
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryBeacon: IPv4 socket unavailable");
    if (!openSock6())
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryBeacon: IPv6 socket unavailable");
}

DiscoveryBeacon::~DiscoveryBeacon() {
#if defined(_WIN32)
    if (m_sock4 != INVALID_SOCKET) {
        closesocket(m_sock4);
        m_sock4 = INVALID_SOCKET;
    }
    if (m_sock6 != INVALID_SOCKET) {
        closesocket(m_sock6);
        m_sock6 = INVALID_SOCKET;
    }
    if (m_wsaOwner)
        WSACleanup();
#else
    if (m_sock4 >= 0) {
        ::close(m_sock4);
        m_sock4 = -1;
    }
    if (m_sock6 >= 0) {
        ::close(m_sock6);
        m_sock6 = -1;
    }
#endif
}

bool DiscoveryBeacon::isOpen() const noexcept {
#if defined(_WIN32)
    return m_sock4 != INVALID_SOCKET || m_sock6 != INVALID_SOCKET;
#else
    return m_sock4 >= 0 || m_sock6 >= 0;
#endif
}

void DiscoveryBeacon::tick(int playerCount) {
    if (!isOpen())
        return;
    if (m_firstTick) {
        m_firstTick = false;
        m_lastSend = std::chrono::steady_clock::now();
        send(playerCount);
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSend).count();
    if (elapsed >= static_cast<long long>(m_cfg.intervalMs)) {
        m_lastSend = now;
        send(playerCount);
    }
}

bool DiscoveryBeacon::openSock4() {
#if defined(_WIN32)
    m_sock4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock4 == INVALID_SOCKET)
        return false;
    int opt = 1;
    if (setsockopt(m_sock4, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
        closesocket(m_sock4);
        m_sock4 = INVALID_SOCKET;
        return false;
    }
#else
    m_sock4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock4 < 0)
        return false;
    int opt = 1;
    if (setsockopt(m_sock4, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
        ::close(m_sock4);
        m_sock4 = -1;
        return false;
    }
#endif
    return true;
}

bool DiscoveryBeacon::openSock6() {
#if defined(_WIN32)
    m_sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (m_sock6 == INVALID_SOCKET)
        return false;
    int hops = 1;
    if (setsockopt(m_sock6, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, reinterpret_cast<const char*>(&hops), sizeof(hops)) !=
        0) {
        closesocket(m_sock6);
        m_sock6 = INVALID_SOCKET;
        return false;
    }
    DWORD ifIdx = 0;
    setsockopt(m_sock6, IPPROTO_IPV6, IPV6_MULTICAST_IF, reinterpret_cast<const char*>(&ifIdx), sizeof(ifIdx));
#else
    m_sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (m_sock6 < 0)
        return false;
    int hops = 1;
    if (setsockopt(m_sock6, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, reinterpret_cast<const char*>(&hops), sizeof(hops)) !=
        0) {
        ::close(m_sock6);
        m_sock6 = -1;
        return false;
    }
    unsigned ifIdx = 0;
    setsockopt(m_sock6, IPPROTO_IPV6, IPV6_MULTICAST_IF, reinterpret_cast<const char*>(&ifIdx), sizeof(ifIdx));
#endif
    return true;
}

void DiscoveryBeacon::send(int playerCount) {
    fl::MsgLanBeacon pkt;
    pkt.protocolVersion = fl::kProtocolVersion;
    pkt.gamePort = m_cfg.port;
    pkt.playerCount = static_cast<uint8_t>(std::clamp(playerCount, 0, 255));
    pkt.maxPlayers = m_cfg.maxPlayers;
    pkt.gameModeFlags = m_cfg.gameModeFlags;
    std::snprintf(pkt.name, sizeof(pkt.name), "%s", m_cfg.name.c_str());

    uint8_t buf[sizeof(fl::MsgLanBeacon)];
    std::memcpy(buf, &pkt, sizeof(pkt));

    // IPv4 broadcast
#if defined(_WIN32)
    if (m_sock4 != INVALID_SOCKET) {
#else
    if (m_sock4 >= 0) {
#endif
        sockaddr_in d4{};
        d4.sin_family = AF_INET;
        d4.sin_port = htons(m_cfg.port);
        inet_pton(AF_INET, m_cfg.broadcastAddr.c_str(), &d4.sin_addr);
        if (sendto(m_sock4, reinterpret_cast<const char*>(buf), static_cast<int>(sizeof(buf)), 0,
                   reinterpret_cast<const sockaddr*>(&d4), static_cast<int>(sizeof(d4))) < 0)
            m_log->log(LogLevel::Warn, __FILE__, __LINE__, "DiscoveryBeacon: IPv4 sendto failed");
    }

    // IPv6 link-local multicast — ff02::1 (all-nodes on link, no join needed for sender)
#if defined(_WIN32)
    if (m_sock6 != INVALID_SOCKET) {
#else
    if (m_sock6 >= 0) {
#endif
        sockaddr_in6 d6{};
        d6.sin6_family = AF_INET6;
        d6.sin6_port = htons(m_cfg.port);
        d6.sin6_scope_id = 0;
        inet_pton(AF_INET6, "ff02::1", &d6.sin6_addr);
        if (sendto(m_sock6, reinterpret_cast<const char*>(buf), static_cast<int>(sizeof(buf)), 0,
                   reinterpret_cast<const sockaddr*>(&d6), static_cast<int>(sizeof(d6))) < 0)
            m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryBeacon: IPv6 sendto failed (no IPv6 link?)");
    }
}

} // namespace fl
