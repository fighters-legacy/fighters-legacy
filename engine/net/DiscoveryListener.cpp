// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "DiscoveryListener.h"

#include <ILogger.h>
#include <algorithm>
#include <cstring>

#if defined(_WIN32)
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

#if defined(_WIN32)
void setNonBlocking(SOCKET sock) {
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
}
#else
void setNonBlocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}
#endif

bool wouldBlock() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Format a sockaddr as a dotted-decimal or colon-hex IP string.
std::string formatAddr(const sockaddr* sa) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (sa->sa_family == AF_INET) {
        const auto* s4 = reinterpret_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, &s4->sin_addr, buf, sizeof(buf));
    } else if (sa->sa_family == AF_INET6) {
        const auto* s6 = reinterpret_cast<const sockaddr_in6*>(sa);
        inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf));
    }
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// DiscoveryListener
// ---------------------------------------------------------------------------

DiscoveryListener::DiscoveryListener(uint16_t port, ILogger& log, int ttlMs)
    : m_port(port), m_ttlMs(ttlMs), m_log(&log) {
#if defined(_WIN32)
    WSADATA wsa{};
    int err = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (err == 0)
        m_wsaOwner = true;
#endif
    if (!openSock4(port))
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryListener: IPv4 socket unavailable");
    if (!openSock6(port))
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryListener: IPv6 socket unavailable");
}

DiscoveryListener::~DiscoveryListener() {
#if defined(_WIN32)
    if (m_sock4 != INVALID_SOCKET) {
        closesocket(m_sock4);
        m_sock4 = INVALID_SOCKET;
    }
    if (m_sock6 != INVALID_SOCKET) {
        // Leave multicast group before closing.
        struct ipv6_mreq mreq{};
        inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
        mreq.ipv6mr_interface = 0;
        setsockopt(m_sock6, IPPROTO_IPV6, IPV6_LEAVE_GROUP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
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
        struct ipv6_mreq mreq{};
        inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
        mreq.ipv6mr_interface = 0;
        setsockopt(m_sock6, IPPROTO_IPV6, IPV6_LEAVE_GROUP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
        ::close(m_sock6);
        m_sock6 = -1;
    }
#endif
}

bool DiscoveryListener::isOpen() const noexcept {
#if defined(_WIN32)
    return m_sock4 != INVALID_SOCKET || m_sock6 != INVALID_SOCKET;
#else
    return m_sock4 >= 0 || m_sock6 >= 0;
#endif
}

bool DiscoveryListener::openSock4(uint16_t port) {
#if defined(_WIN32)
    m_sock4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock4 == INVALID_SOCKET)
        return false;
    int reuse = 1;
    setsockopt(m_sock4, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock4, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(m_sock4);
        m_sock4 = INVALID_SOCKET;
        return false;
    }
#else
    m_sock4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock4 < 0)
        return false;
    int reuse = 1;
    setsockopt(m_sock4, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(m_sock4, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock4, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(m_sock4);
        m_sock4 = -1;
        return false;
    }
#endif
    setNonBlocking(m_sock4);
    return true;
}

bool DiscoveryListener::openSock6(uint16_t port) {
#if defined(_WIN32)
    m_sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (m_sock6 == INVALID_SOCKET)
        return false;
    int v6only = 1;
    setsockopt(m_sock6, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    int reuse = 1;
    setsockopt(m_sock6, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;
    if (bind(m_sock6, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(m_sock6);
        m_sock6 = INVALID_SOCKET;
        return false;
    }
    struct ipv6_mreq mreq{};
    inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = 0;
    if (setsockopt(m_sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
        closesocket(m_sock6);
        m_sock6 = INVALID_SOCKET;
        return false;
    }
#else
    m_sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (m_sock6 < 0)
        return false;
    int v6only = 1;
    setsockopt(m_sock6, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    int reuse = 1;
    setsockopt(m_sock6, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(m_sock6, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;
    if (bind(m_sock6, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(m_sock6);
        m_sock6 = -1;
        return false;
    }
    struct ipv6_mreq mreq{};
    inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = 0;
    if (setsockopt(m_sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
        ::close(m_sock6);
        m_sock6 = -1;
        return false;
    }
#endif
    setNonBlocking(m_sock6);
    return true;
}

void DiscoveryListener::poll() {
#if defined(_WIN32)
    if (m_sock4 != INVALID_SOCKET)
        drainSock(m_sock4, false);
    if (m_sock6 != INVALID_SOCKET)
        drainSock(m_sock6, true);
#else
    if (m_sock4 >= 0)
        drainSock(m_sock4, false);
    if (m_sock6 >= 0)
        drainSock(m_sock6, true);
#endif

    // Prune stale entries
    auto now = std::chrono::steady_clock::now();
    m_servers.erase(
        std::remove_if(m_servers.begin(), m_servers.end(),
                       [&](const ServerInfo& s) {
                           auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s.lastSeen).count();
                           return ageMs > static_cast<long long>(m_ttlMs);
                       }),
        m_servers.end());
}

std::vector<DiscoveryListener::ServerInfo> DiscoveryListener::servers() const {
    return m_servers;
}

#if defined(_WIN32)
void DiscoveryListener::drainSock(SOCKET sock, bool isIPv6) {
#else
void DiscoveryListener::drainSock(int sock, bool isIPv6) {
#endif
    uint8_t buf[256];
    sockaddr_in6 src{}; // large enough for both IPv4 and IPv6
    SockLen srcLen = sizeof(src);

    for (;;) {
        int n = static_cast<int>(recvfrom(sock, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0,
                                          reinterpret_cast<sockaddr*>(&src), &srcLen));
        if (n < 0) {
            if (wouldBlock())
                break;
            // Other error — stop draining
            break;
        }
        // Validate minimum size and msgId
        if (static_cast<std::size_t>(n) < sizeof(fl::MsgLanBeacon))
            continue;
        if (buf[0] != static_cast<uint8_t>(fl::MsgId::LanBeacon))
            continue;

        fl::MsgLanBeacon pkt;
        std::memcpy(&pkt, buf, sizeof(pkt));

        // Format source address
        std::string srcAddr = formatAddr(reinterpret_cast<const sockaddr*>(&src));

        // Deduplication key: (gamePort, name) — same server broadcasts on both IPv4 and IPv6.
        std::string name(pkt.name, std::find(pkt.name, pkt.name + sizeof(pkt.name), '\0'));
        auto it = std::find_if(m_servers.begin(), m_servers.end(), [&](const ServerInfo& s) {
            std::string sn(s.beacon.name, std::find(s.beacon.name, s.beacon.name + sizeof(s.beacon.name), '\0'));
            return s.beacon.gamePort == pkt.gamePort && sn == name;
        });

        auto now = std::chrono::steady_clock::now();

        if (it == m_servers.end()) {
            // New server
            ServerInfo info;
            info.beacon = pkt;
            info.address = srcAddr;
            info.lastSeen = now;
            m_servers.push_back(std::move(info));

            char msg[160];
            std::snprintf(msg, sizeof(msg), "LAN server discovered: \"%s\" at %s port %u (%u/%u players)", pkt.name,
                          srcAddr.c_str(), static_cast<unsigned>(pkt.gamePort), static_cast<unsigned>(pkt.playerCount),
                          static_cast<unsigned>(pkt.maxPlayers));
            m_log->log(LogLevel::Info, __FILE__, __LINE__, msg);
        } else {
            // Update existing entry; prefer IPv4 address over IPv6 link-local
            it->beacon = pkt;
            it->lastSeen = now;
            bool newIsIPv4 = (srcAddr.find(':') == std::string::npos);
            bool existingIsIPv6 = (it->address.find(':') != std::string::npos);
            if (it->address.empty() || (newIsIPv4 && existingIsIPv6))
                it->address = srcAddr;
        }
        (void)isIPv6; // used implicitly through src address family
    }
}
