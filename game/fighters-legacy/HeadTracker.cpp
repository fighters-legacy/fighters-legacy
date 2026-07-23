// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "HeadTracker.h"

namespace fl {

namespace {
#if defined(_WIN32)
constexpr unsigned long long kBadSock = ~0ull;
bool sockValid(unsigned long long s) {
    return s != kBadSock;
}
#else
bool sockValid(int s) {
    return s >= 0;
}
#endif
} // namespace

HeadTracker::~HeadTracker() {
    stop();
}

bool HeadTracker::start(uint16_t port) {
    stop();
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        m_wsaOwner = true;
#endif
    m_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!sockValid(m_sock)) {
        stop();
        return false;
    }
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(m_sock, FIONBIO, &mode);
#else
    const int flags = fcntl(m_sock, F_GETFL, 0);
    fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // opentrack streams to localhost
    if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        stop();
        return false; // port already in use etc. — non-fatal, head tracking just stays off
    }
    m_running = true;
    return true;
}

void HeadTracker::stop() {
    if (sockValid(m_sock)) {
#if defined(_WIN32)
        closesocket(m_sock);
        m_sock = kBadSock;
#else
        ::close(m_sock);
        m_sock = -1;
#endif
    }
#if defined(_WIN32)
    if (m_wsaOwner) {
        WSACleanup();
        m_wsaOwner = false;
    }
#endif
    m_running = false;
}

void HeadTracker::poll(float dt, const HeadTrackingSettings& cfg) {
    RawHeadPose latest;
    bool haveLatest = false;
    if (m_running) {
        // Drain to the newest datagram — an old queued frame is stale for a 60 Hz view.
        for (;;) {
            unsigned char buf[64];
            const auto n = recvfrom(m_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0)
                break;
            if (auto p = parseOpentrackDatagram(buf, static_cast<std::size_t>(n))) {
                latest = *p;
                haveLatest = true;
            }
        }
    }
    m_filter.update(haveLatest ? &latest : nullptr, dt, cfg);
}

} // namespace fl
