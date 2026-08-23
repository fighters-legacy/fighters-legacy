// SPDX-License-Identifier: GPL-3.0-or-later

// The socket compat layer is shared (#1256): platform/SocketCompat.h, which bot_swarm can reach
// because it lives in platform/ ROOT -- this tool deliberately links only platform-net/platform-hal
// and engine-protocol, so an engine/net home would have been unreachable from here.
//
// The POLL layer stays local, as it does in RconServer: only these two files poll a socket set.
//
// ⚠ SocketCompat.h must come FIRST: it carries the winsock2-before-windows include order.
#include <SocketCompat.h>

#if defined(_WIN32)
using PollFd = WSAPOLLFD;
#define PROXY_POLL WSAPoll
#else
#include <poll.h>
using PollFd = struct pollfd;
#define PROXY_POLL ::poll
#endif

using ProxySocket = fl::socket_t;

#include "LossyProxy.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <queue>
#include <thread>
#include <vector>

namespace fl {

namespace {

using Clock = std::chrono::steady_clock;

// One datagram held back by the delay policy, keyed by its release time (min-heap).
struct DelayedPacket {
    Clock::time_point release;
    bool toServer{false};  // direction: true = client->server, false = server->client
    uint64_t clientKey{0}; // which client lane this datagram belongs to
    std::vector<uint8_t> data;
};
struct DelayedLater {
    bool operator()(const DelayedPacket& a, const DelayedPacket& b) const {
        return a.release > b.release;
    }
};

inline uint64_t keyOf(const sockaddr_in& a) {
    return (static_cast<uint64_t>(a.sin_addr.s_addr) << 16) | a.sin_port;
}

} // namespace

struct LossyProxy::Impl {
    ProxySocket listenSock{fl::kInvalidSocket};
    sockaddr_in serverAddr{};
    uint16_t port{0};

    // One connected upstream socket per distinct client source (so fl-server sees N peers).
    struct ClientLane {
        ProxySocket up{fl::kInvalidSocket};
        sockaddr_in addr{}; // the client's source addr, for the return path
    };
    std::map<uint64_t, ClientLane> clients; // I/O-thread only after start()

    LossyLink link{LossySchedule{}};
    Clock::time_point t0{};
    std::priority_queue<DelayedPacket, std::vector<DelayedPacket>, DelayedLater> delayed; // I/O-thread only

    std::thread ioThread;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> forwarded{0};
    std::atomic<uint64_t> dropped{0};

    double nowS() const {
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    void sendToServer(uint64_t clientKey, const uint8_t* data, std::size_t len) {
        auto it = clients.find(clientKey);
        if (it == clients.end())
            return;
        (void)send(it->second.up, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
        forwarded.fetch_add(1, std::memory_order_relaxed);
    }

    void sendToClient(uint64_t clientKey, const uint8_t* data, std::size_t len) {
        auto it = clients.find(clientKey);
        if (it == clients.end())
            return;
        (void)sendto(listenSock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                     reinterpret_cast<const sockaddr*>(&it->second.addr), sizeof(it->second.addr));
        forwarded.fetch_add(1, std::memory_order_relaxed);
    }

    // Route one datagram through the policy: drop, forward now, or park in the delay heap.
    void route(bool toServer, uint64_t clientKey, const uint8_t* data, std::size_t len) {
        const auto verdict = link.classify(nowS());
        if (!verdict) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (*verdict == 0) {
            toServer ? sendToServer(clientKey, data, len) : sendToClient(clientKey, data, len);
            return;
        }
        DelayedPacket p;
        p.release = Clock::now() + std::chrono::milliseconds(*verdict);
        p.toServer = toServer;
        p.clientKey = clientKey;
        p.data.assign(data, data + len);
        delayed.push(std::move(p));
    }

    void releaseDue() {
        const auto now = Clock::now();
        while (!delayed.empty() && delayed.top().release <= now) {
            const DelayedPacket& p = delayed.top();
            p.toServer ? sendToServer(p.clientKey, p.data.data(), p.data.size())
                       : sendToClient(p.clientKey, p.data.data(), p.data.size());
            delayed.pop();
        }
    }

    void run() {
        std::vector<uint8_t> buf(65536);
        std::vector<PollFd> fds;
        std::vector<uint64_t> fdKeys; // parallel to fds; entry 0 (listen) unused
        while (running.load(std::memory_order_relaxed)) {
            fds.clear();
            fdKeys.clear();
            fds.push_back(PollFd{listenSock, POLLIN, 0});
            fdKeys.push_back(0);
            for (const auto& [key, lane] : clients) {
                fds.push_back(PollFd{lane.up, POLLIN, 0});
                fdKeys.push_back(key);
            }

            // Wake early enough to release the next delayed datagram on time.
            int timeoutMs = 5;
            if (!delayed.empty()) {
                const auto until =
                    std::chrono::duration_cast<std::chrono::milliseconds>(delayed.top().release - Clock::now()).count();
                timeoutMs = static_cast<int>(until < 0 ? 0 : (until < 5 ? until : 5));
            }
            PROXY_POLL(fds.data(), static_cast<unsigned long>(fds.size()), timeoutMs);
            releaseDue();

            // Client -> server direction: drain the shared listen socket, laning by source addr.
            if (fds[0].revents & POLLIN) {
                for (;;) {
                    sockaddr_in from{};
                    SockLen fromLen = sizeof(from);
                    const auto n =
                        recvfrom(listenSock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
                    if (n <= 0)
                        break;
                    const uint64_t key = keyOf(from);
                    auto it = clients.find(key);
                    if (it == clients.end()) {
                        // First datagram from a new client: open its dedicated upstream lane.
                        ClientLane lane;
                        lane.addr = from;
                        lane.up = socket(AF_INET, SOCK_DGRAM, 0);
                        if (lane.up == fl::kInvalidSocket)
                            continue;
                        if (connect(lane.up, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) != 0) {
                            fl::closeSocket(lane.up);
                            continue;
                        }
                        fl::setNonBlocking(lane.up);
                        it = clients.emplace(key, lane).first;
                    }
                    route(/*toServer=*/true, key, buf.data(), static_cast<std::size_t>(n));
                }
            }

            // Server -> client direction: drain each client's upstream lane.
            for (std::size_t i = 1; i < fds.size(); ++i) {
                if (!(fds[i].revents & POLLIN))
                    continue;
                for (;;) {
                    const auto n =
                        recv(fds[i].fd, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
                    if (n <= 0)
                        break;
                    route(/*toServer=*/false, fdKeys[i], buf.data(), static_cast<std::size_t>(n));
                }
            }
        }
    }
};

LossyProxy::LossyProxy() : m_impl(std::make_unique<Impl>()) {}

LossyProxy::~LossyProxy() {
    stop();
}

bool LossyProxy::start(const char* serverHost, uint16_t serverPort, const LossySchedule& sched, uint32_t seed) {
    // OS-refcounted; WSAEALREADY (ENet started it) is fine. Held for the proxy's lifetime rather
    // than started here and never released, which is what the hand-rolled call did.
    static fl::WsaGuard s_wsa;
    Impl& im = *m_impl;
    im.link = LossyLink(sched, seed);

    im.serverAddr = {};
    im.serverAddr.sin_family = AF_INET;
    im.serverAddr.sin_port = htons(serverPort);
    if (inet_pton(AF_INET, serverHost, &im.serverAddr.sin_addr) != 1)
        return false; // IPv4 loopback instrument only — hostnames/IPv6 not supported

    im.listenSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (im.listenSock == fl::kInvalidSocket)
        return false;
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = 0; // ephemeral
    inet_pton(AF_INET, "127.0.0.1", &bindAddr.sin_addr);
    if (bind(im.listenSock, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        fl::closeSocket(im.listenSock);
        im.listenSock = fl::kInvalidSocket;
        return false;
    }
    sockaddr_in bound{};
    SockLen boundLen = sizeof(bound);
    if (getsockname(im.listenSock, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        fl::closeSocket(im.listenSock);
        im.listenSock = fl::kInvalidSocket;
        return false;
    }
    im.port = ntohs(bound.sin_port);
    fl::setNonBlocking(im.listenSock);

    im.t0 = Clock::now();
    im.running.store(true, std::memory_order_relaxed);
    im.ioThread = std::thread([this]() { m_impl->run(); });
    return true;
}

void LossyProxy::stop() {
    Impl& im = *m_impl;
    if (im.running.exchange(false, std::memory_order_relaxed)) {
        if (im.ioThread.joinable())
            im.ioThread.join();
    }
    for (auto& [key, lane] : im.clients) {
        (void)key;
        if (lane.up != fl::kInvalidSocket)
            fl::closeSocket(lane.up);
    }
    im.clients.clear();
    if (im.listenSock != fl::kInvalidSocket) {
        fl::closeSocket(im.listenSock);
        im.listenSock = fl::kInvalidSocket;
    }
}

uint16_t LossyProxy::listenPort() const {
    return m_impl->port;
}

uint64_t LossyProxy::forwardedCount() const {
    return m_impl->forwarded.load(std::memory_order_relaxed);
}

uint64_t LossyProxy::droppedCount() const {
    return m_impl->dropped.load(std::memory_order_relaxed);
}

} // namespace fl
