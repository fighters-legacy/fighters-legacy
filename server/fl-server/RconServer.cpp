// SPDX-License-Identifier: GPL-3.0-or-later

// ---------------------------------------------------------------------------
// Platform socket includes — all #ifdefs confined to this translation unit.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using RconSocket = SOCKET;
using SockLen = int;
using PollFd = WSAPOLLFD;
static constexpr RconSocket kInvalidSocket = INVALID_SOCKET;
// WSAStartup is already called by ENet (enet_initialize) before RconServer::start().
#define RCON_POLL WSAPoll
static void rconClose(RconSocket s) {
    closesocket(s);
}
static bool rconWouldBlock() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}
static void rconSetNonBlocking(RconSocket s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using RconSocket = int;
using SockLen = socklen_t;
using PollFd = struct pollfd;
static constexpr RconSocket kInvalidSocket = -1;
#define RCON_POLL ::poll
static void rconClose(RconSocket s) {
    close(s);
}
static bool rconWouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
static void rconSetNonBlocking(RconSocket s) {
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
}
#endif

#include "RconServer.h"
#include <console/CommandRegistry.h>
#include <console/CommandShell.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// rcon free-function implementations
// ---------------------------------------------------------------------------

namespace rcon {

std::vector<uint8_t> encodePacket(int32_t id, int32_t type, std::string_view body) {
    // Packet layout: [size:4LE][id:4LE][type:4LE][body][NUL][NUL]
    // size = 8 (id+type) + body.size() + 2 (NUL pair)
    auto bodyLen = static_cast<int32_t>(body.size());
    int32_t size = 10 + bodyLen;
    std::vector<uint8_t> pkt(static_cast<std::size_t>(4 + size));
    std::memcpy(pkt.data(), &size, 4);
    std::memcpy(pkt.data() + 4, &id, 4);
    std::memcpy(pkt.data() + 8, &type, 4);
    if (bodyLen > 0)
        std::memcpy(pkt.data() + 12, body.data(), static_cast<std::size_t>(bodyLen));
    pkt[12 + bodyLen] = 0; // body NUL terminator
    pkt[13 + bodyLen] = 0; // trailing empty-string NUL
    return pkt;
}

int decodePacket(const uint8_t* buf, int len, RconPacket& out) {
    if (len < 4)
        return 0; // need more data

    int32_t size = 0;
    std::memcpy(&size, buf, 4);

    // Minimum packet: id(4) + type(4) + NUL(1) + NUL(1) = 10
    // Maximum allowed body: kMaxBodyPerPacket bytes → size ≤ 10 + kMaxBodyPerPacket
    if (size < 10 || size > 10 + kMaxBodyPerPacket)
        return -1; // malformed

    int total = 4 + size;
    if (len < total)
        return 0; // incomplete

    std::memcpy(&out.id, buf + 4, 4);
    std::memcpy(&out.type, buf + 8, 4);

    // Body is NUL-terminated, starting at offset 12.  The body region is
    // (size - 10) bytes plus the two NUL terminators = size - 8 bytes total.
    int bodyMax = size - 10; // guaranteed >= 0 by the check above
    const char* bodyStart = reinterpret_cast<const char*>(buf + 12);
    // strnlen-equivalent: find first NUL within the body region
    int bodyLen = 0;
    while (bodyLen < bodyMax && bodyStart[bodyLen] != '\0')
        ++bodyLen;
    out.body.assign(bodyStart, static_cast<std::size_t>(bodyLen));

    return total;
}

std::vector<std::string> splitResponse(std::string_view body) {
    if (body.empty())
        return {""};

    std::vector<std::string> chunks;
    while (!body.empty()) {
        std::size_t n = (body.size() > static_cast<std::size_t>(kMaxBodyPerPacket))
                            ? static_cast<std::size_t>(kMaxBodyPerPacket)
                            : body.size();
        chunks.emplace_back(body.data(), n);
        body.remove_prefix(n);
    }
    return chunks;
}

} // namespace rcon

// ---------------------------------------------------------------------------
// Per-client state
// ---------------------------------------------------------------------------

namespace {

static constexpr int kDrainDelayMs = 20; // slightly more than one 60 Hz sim tick (~16.67 ms)

struct ClientState {
    RconSocket fd = kInvalidSocket;
    std::string address;
    std::vector<uint8_t> recvBuf;
    std::vector<uint8_t> sendBuf;
    enum class Auth { Unauthenticated, Authenticated } authState = Auth::Unauthenticated;
    bool hasPendingDrain = false;
    int drainMark = 0;
    int32_t drainPacketId = 0;
    std::chrono::steady_clock::time_point drainDeadline{};
};

// Append data to client's send buffer. Returns false if the send buffer is
// unreasonably large (client is not draining — close it).
bool queueSend(ClientState& c, std::vector<uint8_t> data) {
    c.sendBuf.insert(c.sendBuf.end(), data.begin(), data.end());
    return c.sendBuf.size() < 256 * 1024; // 256 KB safety cap
}

} // namespace

// ---------------------------------------------------------------------------
// RconServer::Impl
// ---------------------------------------------------------------------------

struct RconServer::Impl {
    const CommandRegistry& m_registry;
    ServerConfig::RconConfig m_cfg;
    ILogger& m_log;
    CommandShell* m_shell;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    RconSocket m_listenSock = kInvalidSocket;
    std::mutex m_authTrackerMutex;
    fl::AuthTracker m_authTracker;

    Impl(const CommandRegistry& reg, const ServerConfig::RconConfig& cfg, ILogger& log, CommandShell* shell)
        : m_registry(reg), m_cfg(cfg), m_log(log), m_shell(shell),
          m_authTracker(cfg.maxAuthFailures, cfg.lockoutSeconds) {}

    void ioLoop();
};

// ---------------------------------------------------------------------------
// ioLoop — background thread
// ---------------------------------------------------------------------------

void RconServer::Impl::ioLoop() {
    static constexpr int kMaxClients = 4;
    std::vector<ClientState> clients;
    clients.reserve(kMaxClients);

    while (m_running.load(std::memory_order_relaxed)) {
        // Build pollfd array: slot 0 = listen socket, slots 1..N = clients.
        std::vector<PollFd> fds;
        fds.reserve(1 + clients.size());

        PollFd lfd{};
        lfd.fd = m_listenSock;
        lfd.events = POLLIN;
        fds.push_back(lfd);

        for (const auto& c : clients) {
            PollFd cfd{};
            cfd.fd = c.fd;
            cfd.events = POLLIN;
            if (!c.sendBuf.empty())
                cfd.events = static_cast<short>(cfd.events | POLLOUT);
            fds.push_back(cfd);
        }

        int pollTimeoutMs = 100;
        if (m_shell) {
            auto now = std::chrono::steady_clock::now();
            for (const auto& c : clients) {
                if (!c.hasPendingDrain || c.fd == kInvalidSocket)
                    continue;
                auto rawMs = std::chrono::duration_cast<std::chrono::milliseconds>(c.drainDeadline - now).count();
                int clampedMs = rawMs <= 0 ? 0 : static_cast<int>(rawMs);
                pollTimeoutMs = std::min(pollTimeoutMs, clampedMs);
            }
        }
        int ready = RCON_POLL(fds.data(), static_cast<unsigned int>(fds.size()), pollTimeoutMs);
        if (ready < 0) {
            if (rconWouldBlock())
                continue;
            break; // fatal poll error
        }
        if (m_shell) {
            auto now = std::chrono::steady_clock::now();
            for (auto& c : clients) {
                if (!c.hasPendingDrain || c.fd == kInvalidSocket)
                    continue;
                if (now < c.drainDeadline)
                    continue;
                c.hasPendingDrain = false;
                auto lines = m_shell->drainSince(c.drainMark);
                if (lines.empty())
                    continue;
                std::string combined;
                for (const auto& l : lines) {
                    if (!combined.empty())
                        combined += '\n';
                    combined += l;
                }
                auto chunks = rcon::splitResponse(combined);
                for (const auto& chunk : chunks)
                    queueSend(c, rcon::encodePacket(c.drainPacketId, rcon::kTypeResponseValue, chunk));
                if (chunks.size() > 1)
                    queueSend(c, rcon::encodePacket(c.drainPacketId, rcon::kTypeResponseValue, ""));
            }
        }
        if (ready == 0) {
            {
                std::lock_guard<std::mutex> lk(m_authTrackerMutex);
                m_authTracker.pruneExpired();
            }
            continue; // timeout — loop back and check m_running
        }

        // --- listen socket ---
        if (fds[0].revents & POLLIN) {
            sockaddr_storage addr{};
            SockLen addrLen = sizeof(addr);
            RconSocket clientFd = accept(m_listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen);
            if (clientFd != kInvalidSocket) {
                // Extract peer IP. Server binds AF_INET; addr is always sockaddr_in.
                char ipBuf[INET6_ADDRSTRLEN] = {};
                const auto* sin = reinterpret_cast<const sockaddr_in*>(&addr);
                inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
                std::string peerIp(ipBuf);

                bool lockedOut;
                {
                    std::lock_guard<std::mutex> lk(m_authTrackerMutex);
                    lockedOut = m_authTracker.isLockedOut(peerIp);
                }
                if (lockedOut) {
                    // Reject before consuming a slot: AUTH_RESPONSE id=-1 then close.
                    auto pkt = rcon::encodePacket(-1, rcon::kTypeAuthResponse, "");
                    send(clientFd, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
                    rconClose(clientFd);
                    m_log.log(LogLevel::Info, __FILE__, __LINE__, "RCON: rejected locked-out IP");
                } else if (static_cast<int>(clients.size()) >= kMaxClients) {
                    // Too many connections: send a polite error and close.
                    auto pkt = rcon::encodePacket(0, rcon::kTypeResponseValue, "too many connections\n");
                    send(clientFd, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
                    rconClose(clientFd);
                } else {
                    rconSetNonBlocking(clientFd);
#if defined(SO_NOSIGPIPE)
                    // macOS: MSG_NOSIGNAL is not available; use socket option instead.
                    int noSigPipe = 1;
                    setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&noSigPipe),
                               sizeof(noSigPipe));
#endif
                    ClientState cs;
                    cs.fd = clientFd;
                    cs.address = peerIp;
                    clients.push_back(std::move(cs));
                    m_log.log(LogLevel::Info, __FILE__, __LINE__, "RCON: client connected");
                }
            }
        }

        // --- client sockets ---
        bool needClean = false;
        for (std::size_t i = 0; i < clients.size(); ++i) {
            auto& c = clients[i];
            auto& revFd = fds[1 + i];

            // Drain send buffer.
            if ((revFd.revents & POLLOUT) && !c.sendBuf.empty()) {
#if defined(MSG_NOSIGNAL)
                int flags = MSG_NOSIGNAL;
#else
                int flags = 0;
#endif
                int sent = static_cast<int>(send(c.fd, reinterpret_cast<const char*>(c.sendBuf.data()),
                                                 static_cast<int>(c.sendBuf.size()), flags));
                if (sent > 0)
                    c.sendBuf.erase(c.sendBuf.begin(), c.sendBuf.begin() + sent);
                else if (sent < 0 && !rconWouldBlock()) {
                    rconClose(c.fd);
                    c.fd = kInvalidSocket;
                    needClean = true;
                    continue;
                }
            }

            if (!(revFd.revents & POLLIN))
                continue;

            // Receive data.
            uint8_t tmp[4096];
            int n = static_cast<int>(recv(c.fd, reinterpret_cast<char*>(tmp), static_cast<int>(sizeof(tmp)), 0));
            if (n <= 0) {
                // n == 0: clean close; n < 0 + !wouldBlock: error
                if (n < 0 && rconWouldBlock())
                    continue;
                rconClose(c.fd);
                c.fd = kInvalidSocket;
                needClean = true;
                m_log.log(LogLevel::Info, __FILE__, __LINE__, "RCON: client disconnected");
                continue;
            }
            c.recvBuf.insert(c.recvBuf.end(), tmp, tmp + n);

            // Parse one or more complete packets.
            bool closeClient = false;
            while (!c.recvBuf.empty()) {
                rcon::RconPacket pkt;
                int consumed = rcon::decodePacket(c.recvBuf.data(), static_cast<int>(c.recvBuf.size()), pkt);
                if (consumed == 0)
                    break; // need more data
                if (consumed < 0) {
                    m_log.log(LogLevel::Warn, __FILE__, __LINE__, "RCON: malformed packet; closing client");
                    closeClient = true;
                    break;
                }
                c.recvBuf.erase(c.recvBuf.begin(), c.recvBuf.begin() + consumed);

                // Auth state machine.
                using Auth = ClientState::Auth;
                if (pkt.type == rcon::kTypeAuth) {
                    bool ok = m_cfg.password.empty() || (pkt.body == m_cfg.password);
                    if (ok) {
                        c.authState = Auth::Authenticated;
                        {
                            std::lock_guard<std::mutex> lk(m_authTrackerMutex);
                            m_authTracker.recordSuccess(c.address);
                        }
                        // Send empty RESPONSE_VALUE first (Valve convention), then AUTH_RESPONSE.
                        if (!queueSend(c, rcon::encodePacket(pkt.id, rcon::kTypeResponseValue, ""))) {
                            closeClient = true;
                            break;
                        }
                        if (!queueSend(c, rcon::encodePacket(pkt.id, rcon::kTypeAuthResponse, ""))) {
                            closeClient = true;
                            break;
                        }
                    } else {
                        // Wrong password: AUTH_RESPONSE with id=-1 per Source Engine convention.
                        queueSend(c, rcon::encodePacket(pkt.id, rcon::kTypeResponseValue, ""));
                        queueSend(c, rcon::encodePacket(-1, rcon::kTypeAuthResponse, ""));
                        // Record failure; log if IP is now locked out.
                        bool nowLocked;
                        {
                            std::lock_guard<std::mutex> lk(m_authTrackerMutex);
                            nowLocked = m_authTracker.recordFailure(c.address);
                        }
                        if (nowLocked)
                            m_log.log(LogLevel::Warn, __FILE__, __LINE__,
                                      "RCON: IP locked out after repeated auth failures");
                        // Close after send; mark for cleanup.
                        // We'll let the POLLOUT drain fire first by setting a flag.
                        closeClient = true;
                        break;
                    }
                } else if (pkt.type == rcon::kTypeExecCommand) {
                    if (c.authState != Auth::Authenticated) {
                        // Drop unauthenticated exec attempts.
                        continue;
                    }
                    // dispatch() is const and thread-safe; mutating handlers go through
                    // GameLoop::enqueueSimCallback (also thread-safe).
                    std::string response = m_registry.dispatch(pkt.body);
                    auto chunks = rcon::splitResponse(response);
                    for (const auto& chunk : chunks) {
                        if (!queueSend(c, rcon::encodePacket(pkt.id, rcon::kTypeResponseValue, chunk))) {
                            closeClient = true;
                            break;
                        }
                    }
                    // If response was split, append an empty sentinel so clients know it's done.
                    if (!closeClient && chunks.size() > 1) {
                        if (!queueSend(c, rcon::encodePacket(pkt.id, rcon::kTypeResponseValue, "")))
                            closeClient = true;
                    }
                    // Set up async drain: mark taken AFTER dispatch() to skip sync shell.print()
                    // calls made during dispatch. The RCON thread will check back in kDrainDelayMs
                    // and send any newly-written ring entries as additional RESPONSE_VALUE packets.
                    if (m_shell && !closeClient) {
                        c.drainMark = m_shell->mark();
                        c.drainPacketId = pkt.id;
                        c.hasPendingDrain = true;
                        c.drainDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDrainDelayMs);
                    }
                }
                // Unknown types are silently dropped.
                if (closeClient)
                    break;
            }

            if (closeClient) {
                // Drain any queued sends synchronously before closing.
                while (!c.sendBuf.empty()) {
#if defined(MSG_NOSIGNAL)
                    int flags = MSG_NOSIGNAL;
#else
                    int flags = 0;
#endif
                    int sent = static_cast<int>(send(c.fd, reinterpret_cast<const char*>(c.sendBuf.data()),
                                                     static_cast<int>(c.sendBuf.size()), flags));
                    if (sent <= 0)
                        break;
                    c.sendBuf.erase(c.sendBuf.begin(), c.sendBuf.begin() + sent);
                }
                rconClose(c.fd);
                c.fd = kInvalidSocket;
                needClean = true;
                m_log.log(LogLevel::Info, __FILE__, __LINE__, "RCON: client disconnected");
            }
        }

        if (needClean)
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [](const ClientState& c) { return c.fd == kInvalidSocket; }),
                          clients.end());
    }

    // Close all remaining client connections on exit.
    for (auto& c : clients)
        if (c.fd != kInvalidSocket)
            rconClose(c.fd);
}

// ---------------------------------------------------------------------------
// RconServer public interface
// ---------------------------------------------------------------------------

RconServer::RconServer(const CommandRegistry& registry, const ServerConfig::RconConfig& cfg, ILogger& log,
                       CommandShell* shell)
    : m_impl(std::make_unique<Impl>(registry, cfg, log, shell)) {}

RconServer::~RconServer() {
    stop();
}

bool RconServer::start() {
    m_impl->m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->m_listenSock == kInvalidSocket) {
        m_impl->m_log.log(LogLevel::Error, __FILE__, __LINE__, "RCON: socket() failed");
        return false;
    }

    int reuse = 1;
    setsockopt(m_impl->m_listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    rconSetNonBlocking(m_impl->m_listenSock);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_impl->m_cfg.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_impl->m_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "RCON: bind() failed on port %u", m_impl->m_cfg.port);
        m_impl->m_log.log(LogLevel::Error, __FILE__, __LINE__, buf);
        rconClose(m_impl->m_listenSock);
        m_impl->m_listenSock = kInvalidSocket;
        return false;
    }

    if (listen(m_impl->m_listenSock, 8) != 0) {
        m_impl->m_log.log(LogLevel::Error, __FILE__, __LINE__, "RCON: listen() failed");
        rconClose(m_impl->m_listenSock);
        m_impl->m_listenSock = kInvalidSocket;
        return false;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "RCON: listening on TCP port %u", m_impl->m_cfg.port);
    m_impl->m_log.log(LogLevel::Info, __FILE__, __LINE__, buf);

    m_impl->m_running.store(true, std::memory_order_relaxed);
    m_impl->m_thread = std::thread([this]() { m_impl->ioLoop(); });
    return true;
}

void RconServer::stop() {
    if (!m_impl->m_running.exchange(false))
        return; // already stopped or never started

    // Close the listen socket to unblock poll() on the listen fd.
    if (m_impl->m_listenSock != kInvalidSocket) {
        rconClose(m_impl->m_listenSock);
        m_impl->m_listenSock = kInvalidSocket;
    }

    if (m_impl->m_thread.joinable())
        m_impl->m_thread.join();
}

bool RconServer::clearLockout(const std::string& ip) {
    if (!m_impl)
        return false;
    std::lock_guard<std::mutex> lock(m_impl->m_authTrackerMutex);
    bool wasLocked = m_impl->m_authTracker.isLockedOut(ip);
    m_impl->m_authTracker.clearLockout(ip);
    return wasLocked;
}

fl::AuthLockoutSummary RconServer::getRconAuthSummary() {
    if (!m_impl)
        return {};
    std::lock_guard<std::mutex> lock(m_impl->m_authTrackerMutex);
    fl::AuthLockoutSummary s;
    s.threshold = m_impl->m_authTracker.maxFailures();
    s.entries = m_impl->m_authTracker.failureSummary();
    for (const auto& e : s.entries)
        if (e.lockedOut)
            ++s.activeCount;
    return s;
}
