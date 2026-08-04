// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "net/GameProtocol.h"
#include "net/ServerQueryResponder.h"

#include <ILogger.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace fl {

#if defined(_WIN32)
using SockLen = int;
static bool sockValid(unsigned long long s) {
    return s != ~0ull;
}
#else
using SockLen = socklen_t;
static bool sockValid(int s) {
    return s >= 0;
}
#endif

ServerQueryResponder::ServerQueryResponder(uint16_t queryPort, ILogger& log) : m_port(queryPort), m_log(&log) {}

ServerQueryResponder::~ServerQueryResponder() {
    stop();
}

bool ServerQueryResponder::start() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        m_wsaOwner = true;
#endif
    m_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!sockValid(m_sock)) {
        m_log->log(LogLevel::Warn, __FILE__, __LINE__, "server query: socket() failed");
        return false;
    }
    int reuse = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    // A short receive timeout so the thread can observe the stop flag without a wakeup pipe.
#if defined(_WIN32)
    DWORD tv = 200; // ms
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_usec = 200000;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(m_port);
    if (bind(m_sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
        closesocket(m_sock);
        m_sock = ~0ull;
#else
        close(m_sock);
        m_sock = -1;
#endif
        m_log->log(LogLevel::Warn, __FILE__, __LINE__, "server query: bind failed");
        return false;
    }
    m_open = true;
    m_running = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void ServerQueryResponder::stop() {
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
    if (sockValid(m_sock)) {
#if defined(_WIN32)
        closesocket(m_sock);
        m_sock = ~0ull;
#else
        close(m_sock);
        m_sock = -1;
#endif
    }
    m_open = false;
#if defined(_WIN32)
    if (m_wsaOwner) {
        WSACleanup();
        m_wsaOwner = false;
    }
#endif
}

void ServerQueryResponder::setStaticInfo(StaticInfo info) {
    std::lock_guard<std::mutex> lk(m_infoMutex);
    m_static = std::move(info);
}

void ServerQueryResponder::setDynamicInfo(const DynamicInfo& info) {
    std::lock_guard<std::mutex> lk(m_infoMutex);
    m_dynamic = info;
}

void ServerQueryResponder::run() {
    // Per-source-IP sliding-window rate limiting + a global cap, to bound reflection abuse.
    constexpr int kPerIpPerSec = 4;
    constexpr int kGlobalPerSec = 256;
    std::unordered_map<uint32_t, std::deque<std::chrono::steady_clock::time_point>> perIp;
    std::deque<std::chrono::steady_clock::time_point> global;

    while (m_running.load(std::memory_order_relaxed)) {
        MsgServerQuery q{};
        sockaddr_in src{};
        SockLen slen = sizeof(src);
        const auto n =
            recvfrom(m_sock, reinterpret_cast<char*>(&q), sizeof(q), 0, reinterpret_cast<sockaddr*>(&src), &slen);
        if (n < 0 || static_cast<std::size_t>(n) < sizeof(MsgServerQuery))
            continue; // timeout, short datagram (anti-amplification), or error
        if (q.msgId != static_cast<uint8_t>(MsgId::ServerQuery) || q.protocolVersion != kProtocolVersion)
            continue;

        const auto now = std::chrono::steady_clock::now();
        const auto cutoff = now - std::chrono::seconds(1);
        auto& ipq = perIp[src.sin_addr.s_addr];
        while (!ipq.empty() && ipq.front() < cutoff)
            ipq.pop_front();
        while (!global.empty() && global.front() < cutoff)
            global.pop_front();
        if (static_cast<int>(ipq.size()) >= kPerIpPerSec || static_cast<int>(global.size()) >= kGlobalPerSec)
            continue; // rate limited — drop silently
        ipq.push_back(now);
        global.push_back(now);

        MsgServerInfo info{};
        info.nonce = q.nonce;
        info.clientTimeMs = q.clientTimeMs; // echo verbatim for the client's cross-check
        {
            std::lock_guard<std::mutex> lk(m_infoMutex);
            info.gamePort = m_static.gamePort;
            std::snprintf(info.build, sizeof(info.build), "%s", m_static.buildVersion.c_str()); // #1074
            info.maxPlayers = m_static.maxPlayers;
            info.gameModeFlags = m_static.gameModeFlags;
            if (m_dynamic.shuttingDown)
                info.gameModeFlags |= kGameModeShuttingDown;
            info.shutdownSeconds = m_dynamic.shutdownSeconds;
            info.playerCount = m_dynamic.playerCount;
            std::snprintf(info.name, sizeof(info.name), "%s", m_static.name.c_str());
            std::snprintf(info.modeId, sizeof(info.modeId), "%s", m_static.modeId.c_str());
            std::snprintf(info.mission, sizeof(info.mission), "%s", m_static.mission.c_str());
        }
        sendto(m_sock, reinterpret_cast<const char*>(&info), sizeof(info), 0, reinterpret_cast<const sockaddr*>(&src),
               slen);
    }
}

} // namespace fl
