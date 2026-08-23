// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable raw-socket compatibility, in one place (#1256).
//
// Six translation units re-rolled this: the four discovery/query files in engine/net, fl-server's
// RconServer, and the game's HeadTracker -- plus a seventh partial copy in tools/bot_swarm. Thirty
// `#if defined(_WIN32)` blocks between them, and the drift had already started: the invalid-socket
// sentinel was spelled three different ways (INVALID_SOCKET, a local `kBadSock = ~0ull`, and
// `kInvalidSocket`), and WSAStartup ownership diverged -- five sites take their own reference while
// RconServer takes none at all, relying on ENet having initialised Winsock first.
//
// ⚠ THIS HEADER MUST BE INCLUDED BEFORE ANY OTHER NETWORK HEADER. <winsock2.h> and <windows.h>
// disagree unless winsock2 is first, which is what the WIN32_LEAN_AND_MEAN dance below is for.
//
// It lives in platform/ root rather than engine/net or platform/net. engine/ may include platform/
// root headers and not the reverse, platform/net is on the engine-forbidden backend list in
// cmake/layering.cmake, and bot_swarm links neither engine-net nor anything above it. platform/
// root is the only home all seven consumers can reach.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <string>

namespace fl {

#if defined(_WIN32)
using socket_t = SOCKET;
using SockLen = int;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
using SockLen = socklen_t;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// Windows' INVALID_SOCKET is an unsigned sentinel and POSIX's is any negative fd, so "is this
// socket usable" is not the same test on the two platforms -- which is exactly why three different
// spellings of it grew.
[[nodiscard]] inline bool sockValid(socket_t s) noexcept {
#if defined(_WIN32)
    return s != kInvalidSocket;
#else
    return s >= 0;
#endif
}

inline void closeSocket(socket_t s) noexcept {
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

inline void setNonBlocking(socket_t s) noexcept {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

// True when the last socket operation failed only because it would have blocked -- the normal
// "nothing to read" answer on a non-blocking socket, not an error.
[[nodiscard]] inline bool wouldBlock() noexcept {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Receive timeout, so a blocking recv can notice a stop flag without a wakeup pipe. Windows takes
// a DWORD of milliseconds where POSIX takes a timeval, which is the last real platform difference
// left in the discovery/query files once the rest of this header is in use.
inline void setReceiveTimeoutMs(socket_t s, int ms) noexcept {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// A sockaddr as a dotted-decimal or colon-hex IP string. Empty for an address family we do not
// speak, rather than whatever was left on the stack.
[[nodiscard]] inline std::string formatAddr(const sockaddr* sa) {
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

// Winsock's per-process reference, as RAII, replacing five hand-rolled `m_wsaOwner` flags.
//
// WSAStartup is reference-counted by the OS, so taking a reference costs nothing even when ENet or
// the GNS backend already started Winsock -- and it removes the assumption RconServer relied on,
// that some other component initialised it FIRST. That assumption held only because
// createNetwork() happens to run before rconServer->start(); it would have failed with
// WSANOTINITIALISED the day that order changed.
//
// A no-op on POSIX, so callers need no #if of their own.
class WsaGuard {
  public:
    WsaGuard() noexcept {
#if defined(_WIN32)
        WSADATA wsa{};
        m_owned = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
    }
    ~WsaGuard() {
#if defined(_WIN32)
        if (m_owned)
            WSACleanup();
#endif
    }

    WsaGuard(const WsaGuard&) = delete;
    WsaGuard& operator=(const WsaGuard&) = delete;
    WsaGuard(WsaGuard&&) = delete;
    WsaGuard& operator=(WsaGuard&&) = delete;

  private:
#if defined(_WIN32)
    bool m_owned{false};
#endif
};

} // namespace fl
