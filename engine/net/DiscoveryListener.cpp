// SPDX-License-Identifier: GPL-3.0-or-later
#include "DiscoveryListener.h"

#include <ILogger.h>
#include <algorithm>
#include <cstring>

namespace fl {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// DiscoveryListener
// ---------------------------------------------------------------------------

DiscoveryListener::DiscoveryListener(uint16_t port, ILogger& log, int ttlMs) : m_ttlMs(ttlMs), m_log(&log) {
    if (!openSock4(port))
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryListener: IPv4 socket unavailable");
    if (!openSock6(port))
        m_log->log(LogLevel::Info, __FILE__, __LINE__, "DiscoveryListener: IPv6 socket unavailable");
}

DiscoveryListener::~DiscoveryListener() {
    if (sockValid(m_sock4)) {
        closeSocket(m_sock4);
        m_sock4 = kInvalidSocket;
    }
    if (sockValid(m_sock6)) {
        // Leave the multicast group before closing.
        struct ipv6_mreq mreq{};
        inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
        mreq.ipv6mr_interface = 0;
        setsockopt(m_sock6, IPPROTO_IPV6, IPV6_LEAVE_GROUP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
        closeSocket(m_sock6);
        m_sock6 = kInvalidSocket;
    }
    // m_wsa releases the Winsock reference; on POSIX it is nothing.
}

bool DiscoveryListener::isOpen() const noexcept {
    return sockValid(m_sock4) || sockValid(m_sock6);
}

bool DiscoveryListener::openSock4(uint16_t port) {
    m_sock4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (!sockValid(m_sock4))
        return false;
    int reuse = 1;
    setsockopt(m_sock4, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
    // POSIX only, and deliberately not emulated on Windows: SO_REUSEADDR there already allows the
    // rebind this is for.
    setsockopt(m_sock4, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock4, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(m_sock4);
        m_sock4 = kInvalidSocket;
        return false;
    }
    setNonBlocking(m_sock4);
    return true;
}

bool DiscoveryListener::openSock6(uint16_t port) {
    m_sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (!sockValid(m_sock6))
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
        closeSocket(m_sock6);
        m_sock6 = kInvalidSocket;
        return false;
    }
    struct ipv6_mreq mreq{};
    inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = 0;
    if (setsockopt(m_sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
        closeSocket(m_sock6);
        m_sock6 = kInvalidSocket;
        return false;
    }
    setNonBlocking(m_sock6);
    return true;
}

void DiscoveryListener::poll() {
    if (sockValid(m_sock4))
        drainSock(m_sock4, false);
    if (sockValid(m_sock6))
        drainSock(m_sock6, true);

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

void DiscoveryListener::drainSock(socket_t sock, bool isIPv6) {
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
        // Require only the pre-#1074 prefix: a server built before the build-version tail sends a
        // shorter datagram, and demanding the current sizeof would drop it outright — a browser that
        // silently cannot see older servers is a worse failure than not knowing their build.
        if (static_cast<std::size_t>(n) < fl::kLanBeaconLegacyBytes)
            continue;
        if (buf[0] != static_cast<uint8_t>(fl::MsgId::LanBeacon))
            continue;

        // Copy only what actually arrived. A legacy (pre-#1074) datagram is kLanBeaconLegacyBytes
        // long, so a full-sizeof memcpy would read past the received bytes; the value-initialized
        // struct leaves `build` empty, which is exactly "this server did not advertise one".
        fl::MsgLanBeacon pkt{};
        std::memcpy(&pkt, buf, std::min(static_cast<std::size_t>(n), sizeof(pkt)));
        pkt.build[sizeof(pkt.build) - 1] = '\0'; // untrusted char[]: force-terminate before use

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

} // namespace fl
