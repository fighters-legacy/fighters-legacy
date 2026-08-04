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

#include "net/ServerQueryClient.h"

#include <ILogger.h>
#include <chrono>
#include <cstring>
#include <string>

namespace fl {

#if defined(_WIN32)
using SockLen = int;
static constexpr unsigned long long kBadSock = ~0ull;
static bool sockValid(unsigned long long s) {
    return s != kBadSock;
}
#else
using SockLen = socklen_t;
static bool sockValid(int s) {
    return s >= 0;
}
#endif

ServerQueryClient::ServerQueryClient(ILogger& log, int timeoutMs) : m_log(&log), m_timeoutMs(timeoutMs) {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        m_wsaOwner = true;
    m_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockValid(m_sock)) {
        u_long mode = 1;
        ioctlsocket(m_sock, FIONBIO, &mode);
    }
#else
    m_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock >= 0) {
        int flags = fcntl(m_sock, F_GETFL, 0);
        fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    if (!sockValid(m_sock) && m_log)
        m_log->log(LogLevel::Warn, __FILE__, __LINE__,
                   "ServerQueryClient: failed to open UDP socket; server queries disabled");
}

ServerQueryClient::~ServerQueryClient() {
#if defined(_WIN32)
    if (sockValid(m_sock))
        closesocket(m_sock);
    if (m_wsaOwner)
        WSACleanup();
#else
    if (m_sock >= 0)
        close(m_sock);
#endif
}

bool ServerQueryClient::isOpen() const noexcept {
    return sockValid(m_sock);
}

uint32_t ServerQueryClient::query(const std::string& host, uint16_t queryPort) {
    if (!isOpen())
        return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(queryPort);
    if (inet_pton(AF_INET, host.c_str(), &dst.sin_addr) != 1)
        return 0; // IPv4 literal only (browser resolves lobby hostnames elsewhere)

    const uint32_t nonce = m_nextNonce++;
    if (m_nextNonce == 0)
        m_nextNonce = 1;
    MsgServerQuery q{};
    q.nonce = nonce;
    q.clientTimeMs = 0; // echoed back; RTT uses the local send time, not this
    const auto n = sendto(m_sock, reinterpret_cast<const char*>(&q), sizeof(q), 0,
                          reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (n < 0 || static_cast<std::size_t>(n) != sizeof(q))
        return 0;
    m_pending[nonce] = Pending{host, queryPort, std::chrono::steady_clock::now()};
    return nonce;
}

void ServerQueryClient::poll() {
    if (!isOpen())
        return;
    // Expire stale pending entries.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.sentAt).count() > m_timeoutMs)
            it = m_pending.erase(it);
        else
            ++it;
    }

    for (;;) {
        MsgServerInfo info{};
        sockaddr_in src{};
        SockLen slen = sizeof(src);
        const auto n =
            recvfrom(m_sock, reinterpret_cast<char*>(&info), sizeof(info), 0, reinterpret_cast<sockaddr*>(&src), &slen);
        // Only the pre-#1074 prefix is required, so a server built before the build-version tail is
        // still answered rather than silently dropped (see kServerInfoLegacyBytes).
        if (n < 0 || static_cast<std::size_t>(n) < kServerInfoLegacyBytes)
            break; // no more datagrams (or a short/invalid one)
        if (info.msgId != static_cast<uint8_t>(MsgId::ServerInfo))
            continue;
        const auto pit = m_pending.find(info.nonce);
        if (pit == m_pending.end())
            continue; // unknown/spoofed nonce
        const auto recvAt = std::chrono::steady_clock::now();
        const float rtt = std::chrono::duration<float, std::milli>(recvAt - pit->second.sentAt).count();
        // Force-terminate untrusted strings.
        info.name[sizeof(info.name) - 1] = '\0';
        info.modeId[sizeof(info.modeId) - 1] = '\0';
        info.mission[sizeof(info.mission) - 1] = '\0';
        Result r;
        r.address = pit->second.address;
        r.queryPort = pit->second.queryPort;
        r.info = info;
        r.rttMs = rtt;
        r.receivedAt = recvAt;
        m_results[r.address + ":" + std::to_string(r.queryPort)] = std::move(r);
        m_pending.erase(pit);
    }
}

std::vector<ServerQueryClient::Result> ServerQueryClient::results() const {
    std::vector<Result> out;
    out.reserve(m_results.size());
    for (const auto& [k, v] : m_results) {
        (void)k;
        out.push_back(v);
    }
    return out;
}

void ServerQueryClient::clear() {
    m_pending.clear();
    m_results.clear();
}

} // namespace fl
