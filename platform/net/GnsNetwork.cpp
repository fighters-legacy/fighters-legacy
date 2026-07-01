// SPDX-License-Identifier: GPL-3.0-or-later
#include "GnsNetwork.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

namespace fl {

// -------------------------------------------------------------------------
// Global init refcount — GameNetworkingSockets_Init()/_Kill() are process-global and not
// ref-counted by GNS, so (like ENetNetwork's g_enetRefCount) we init on the first instance and
// kill only on the last, guarded by a mutex. Lets N GnsNetwork instances coexist in one process.
// -------------------------------------------------------------------------
namespace {
std::mutex g_gnsInitMutex;
int g_gnsRefCount = 0;

// The connection-status callback is a per-connection config value; accepted connections inherit it
// (and the ConnectionUserData) from their listen socket. The trampoline recovers the owning
// GnsNetwork from the connection's user data, so there is no process-global registry.
void connStatusTrampoline(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    auto* self = reinterpret_cast<GnsNetwork*>(static_cast<std::intptr_t>(pInfo->m_info.m_nUserData));
    if (self)
        self->onConnectionStatusChanged(pInfo);
}

// Fills the shared connection options: routing callback + owner pointer + insecure-allow flag.
int fillConnectionOptions(SteamNetworkingConfigValue_t opts[3], GnsNetwork* owner, bool allowInsecure) {
    opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   reinterpret_cast<void*>(&connStatusTrampoline));
    opts[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                     static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(owner)));
    opts[2].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, allowInsecure ? 1 : 0);
    return 3;
}
} // namespace

GnsNetwork::~GnsNetwork() {
    if (m_sockets || m_initialized)
        shutdown();
}

bool GnsNetwork::init() {
    if (m_initialized)
        return true;
    std::lock_guard<std::mutex> lock(g_gnsInitMutex);
    if (g_gnsRefCount == 0) {
        SteamNetworkingErrMsg errMsg{};
        if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
            m_lastError = std::string("GameNetworkingSockets_Init failed: ") + errMsg;
            return false;
        }
    }
    ++g_gnsRefCount;
    m_sockets = SteamNetworkingSockets();
    if (!m_sockets) {
        m_lastError = "SteamNetworkingSockets() returned null";
        if (--g_gnsRefCount == 0)
            GameNetworkingSockets_Kill();
        return false;
    }
    m_initialized = true;
    return true;
}

void GnsNetwork::shutdown() {
    if (m_sockets) {
        for (const auto& [conn, peer] : m_connToPeer)
            m_sockets->CloseConnection(conn, 0, "server shutdown", true);
        m_connToPeer.clear();
        m_peerToConn.clear();
        if (m_clientConn) {
            m_sockets->CloseConnection(m_clientConn, 0, "client shutdown", true);
            m_clientConn = 0;
        }
        if (m_pollGroup) {
            m_sockets->DestroyPollGroup(m_pollGroup);
            m_pollGroup = 0;
        }
        if (m_listenSocket) {
            m_sockets->CloseListenSocket(m_listenSocket);
            m_listenSocket = 0;
        }
        m_sockets = nullptr;
    }
    if (m_initialized) {
        std::lock_guard<std::mutex> lock(g_gnsInitMutex);
        if (--g_gnsRefCount == 0)
            GameNetworkingSockets_Kill();
        m_initialized = false;
    }
    m_isServer = false;
    m_handler = nullptr;
}

void GnsNetwork::setEventHandler(INetworkEventHandler* handler) {
    m_handler = handler;
}

// -------------------------------------------------------------------------
// Server / client setup
// -------------------------------------------------------------------------

bool GnsNetwork::bind(const char* address, uint16_t port, int maxClients) {
    if (m_listenSocket || m_clientConn) {
        m_lastError = "already bound or connected";
        return false;
    }
    SteamNetworkingIPAddr addr;
    addr.Clear();
    if (!address || address[0] == '\0' || std::strcmp(address, "0.0.0.0") == 0) {
        addr.SetIPv4(0, port); // INADDR_ANY (IPv4-mapped)
    } else if (std::strcmp(address, "::") == 0) {
        addr.Clear();
        addr.m_port = port; // all-zero IPv6 = in6addr_any
    } else if (!addr.ParseString(address)) {
        // ParseString accepts "ip" or "ip:port"; retry with the port appended for a bare IP.
        std::string withPort = std::string(address) + ":" + std::to_string(port);
        if (!addr.ParseString(withPort.c_str())) {
            m_lastError = "invalid bind address";
            return false;
        }
    } else {
        addr.m_port = port;
    }

    SteamNetworkingConfigValue_t opts[3];
    int nOpts = fillConnectionOptions(opts, this, m_allowInsecure);
    m_listenSocket = m_sockets->CreateListenSocketIP(addr, nOpts, opts);
    if (m_listenSocket == k_HSteamListenSocket_Invalid) {
        m_listenSocket = 0;
        m_lastError = "CreateListenSocketIP failed";
        return false;
    }
    m_pollGroup = m_sockets->CreatePollGroup();
    m_maxClients = maxClients;
    m_isServer = true;
    return true;
}

bool GnsNetwork::connect(const char* host, uint16_t port) {
    if (m_listenSocket || m_clientConn) {
        m_lastError = "already bound or connected";
        return false;
    }
    SteamNetworkingIPAddr addr;
    addr.Clear();
    std::string hostPort = std::string(host) + ":" + std::to_string(port);
    if (!addr.ParseString(host)) {
        if (!addr.ParseString(hostPort.c_str())) {
            m_lastError = "could not parse host address";
            return false;
        }
    }
    addr.m_port = port;

    SteamNetworkingConfigValue_t opts[3];
    int nOpts = fillConnectionOptions(opts, this, m_allowInsecure);
    m_clientConn = m_sockets->ConnectByIPAddress(addr, nOpts, opts);
    if (m_clientConn == k_HSteamNetConnection_Invalid) {
        m_clientConn = 0;
        m_lastError = "ConnectByIPAddress failed";
        return false;
    }
    m_isServer = false;
    return true;
}

void GnsNetwork::disconnect() {
    if (!m_sockets)
        return;
    if (m_clientConn) {
        closeAndErase(m_clientConn, false);
    }
    for (const auto& [conn, peer] : m_connToPeer)
        m_sockets->CloseConnection(conn, 0, "disconnect", true);
    m_connToPeer.clear();
    m_peerToConn.clear();
    if (m_pollGroup) {
        m_sockets->DestroyPollGroup(m_pollGroup);
        m_pollGroup = 0;
    }
    if (m_listenSocket) {
        m_sockets->CloseListenSocket(m_listenSocket);
        m_listenSocket = 0;
    }
    m_isServer = false;
}

// -------------------------------------------------------------------------
// Connection-status callback (from RunCallbacks in service())
// -------------------------------------------------------------------------

void GnsNetwork::onConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t* info) {
    const uint32_t hConn = info->m_hConn;
    switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
        // Inbound (on our listen socket): accept (subject to the maxClients cap) and assign to the
        // poll group. Outbound (client) Connecting fires with no listen socket — wait for Connected.
        if (info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
            if (m_maxClients > 0 && static_cast<int>(m_connToPeer.size()) >= m_maxClients) {
                m_sockets->CloseConnection(hConn, 0, "server full", false);
                return;
            }
            if (m_sockets->AcceptConnection(hConn) != k_EResultOK) {
                m_sockets->CloseConnection(hConn, 0, "accept failed", false);
                return;
            }
            m_sockets->SetConnectionPollGroup(hConn, m_pollGroup);
        }
        break;
    case k_ESteamNetworkingConnectionState_Connected: {
        if (m_connToPeer.find(hConn) == m_connToPeer.end()) {
            const uint32_t peerId = m_nextPeerId++;
            m_peerToConn[peerId] = hConn;
            m_connToPeer[hConn] = peerId;
            if (m_handler)
                m_handler->onConnect(peerId);
        }
        break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        closeAndErase(hConn, true);
        break;
    default:
        break;
    }
}

// -------------------------------------------------------------------------
// Data transfer
// -------------------------------------------------------------------------

bool GnsNetwork::send(uint32_t peerId, const void* data, std::size_t size, bool reliable) {
    if (!m_sockets) {
        m_lastError = "not connected";
        return false;
    }
    const uint32_t conn = m_isServer ? connForPeer(peerId) : m_clientConn;
    if (!conn) {
        m_lastError = "peer not connected";
        return false;
    }
    const int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    const EResult r = m_sockets->SendMessageToConnection(conn, data, static_cast<uint32_t>(size), flags, nullptr);
    if (r != k_EResultOK) {
        m_lastError = "SendMessageToConnection failed";
        return false;
    }
    return true;
}

void GnsNetwork::broadcast(const void* data, std::size_t size, bool reliable) {
    if (!m_sockets)
        return;
    const int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    if (m_isServer) {
        for (const auto& [conn, peer] : m_connToPeer)
            m_sockets->SendMessageToConnection(conn, data, static_cast<uint32_t>(size), flags, nullptr);
    } else if (m_clientConn) {
        m_sockets->SendMessageToConnection(m_clientConn, data, static_cast<uint32_t>(size), flags, nullptr);
    }
}

// -------------------------------------------------------------------------
// Frame pump
// -------------------------------------------------------------------------

void GnsNetwork::service(int timeoutMs) {
    if (!m_sockets)
        return;
    m_sockets->RunCallbacks(); // dispatches onConnectionStatusChanged via the trampoline

    SteamNetworkingMessage_t* msgs[32];
    for (;;) {
        int n = 0;
        if (m_isServer && m_pollGroup)
            n = m_sockets->ReceiveMessagesOnPollGroup(m_pollGroup, msgs, 32);
        else if (!m_isServer && m_clientConn)
            n = m_sockets->ReceiveMessagesOnConnection(m_clientConn, msgs, 32);
        if (n <= 0)
            break;
        for (int i = 0; i < n; ++i) {
            SteamNetworkingMessage_t* m = msgs[i];
            if (m_handler) {
                auto it = m_connToPeer.find(m->m_conn);
                const uint32_t peerId = (it != m_connToPeer.end()) ? it->second : 0u;
                m_handler->onReceive(peerId, m->m_pData, m->m_cbSize);
            }
            m->Release();
        }
        if (n < 32)
            break;
    }

    // GNS receive is non-blocking; mirror ENet's "block up to timeoutMs" so per-frame pumps spread
    // RunCallbacks over wall-clock time (handshakes need it) without busy-spinning. service(0) — the
    // production sim-tick path — never sleeps.
    if (timeoutMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
}

// -------------------------------------------------------------------------
// Peer info
// -------------------------------------------------------------------------

int GnsNetwork::getPeerCount() const {
    return static_cast<int>(m_connToPeer.size());
}

PeerState GnsNetwork::getPeerState(uint32_t peerId) const {
    const uint32_t conn = connForPeer(peerId);
    if (!conn || !m_sockets)
        return PeerState::Disconnected;
    SteamNetConnectionInfo_t info;
    if (!m_sockets->GetConnectionInfo(conn, &info))
        return PeerState::Disconnected;
    switch (info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
    case k_ESteamNetworkingConnectionState_FindingRoute:
        return PeerState::Connecting;
    case k_ESteamNetworkingConnectionState_Connected:
        return PeerState::Connected;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        return PeerState::Disconnecting;
    default:
        return PeerState::Disconnected;
    }
}

const char* GnsNetwork::getPeerAddress(uint32_t peerId) const {
    const uint32_t conn = connForPeer(peerId);
    if (!conn || !m_sockets)
        return nullptr;
    SteamNetConnectionInfo_t info;
    if (!m_sockets->GetConnectionInfo(conn, &info))
        return nullptr;
    char buf[SteamNetworkingIPAddr::k_cchMaxString];
    info.m_addrRemote.ToString(buf, sizeof(buf), true); // "ip:port" / "[ipv6]:port"
    m_peerAddressBuf = buf;
    return m_peerAddressBuf.c_str();
}

void GnsNetwork::disconnectPeer(uint32_t peerId) {
    const uint32_t conn = connForPeer(peerId);
    if (conn)
        closeAndErase(conn, false);
}

const char* GnsNetwork::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

uint32_t GnsNetwork::getPeerRtt(uint32_t peerId) const {
    const uint32_t conn = connForPeer(peerId);
    if (!conn || !m_sockets)
        return 0u;
    SteamNetConnectionRealTimeStatus_t status;
    if (m_sockets->GetConnectionRealTimeStatus(conn, &status, 0, nullptr) != k_EResultOK)
        return 0u;
    return status.m_nPing < 0 ? 0u : static_cast<uint32_t>(status.m_nPing);
}

PeerLinkStats GnsNetwork::getPeerLinkStats(uint32_t peerId) const {
    const uint32_t conn = connForPeer(peerId);
    if (!conn || !m_sockets)
        return {};
    SteamNetConnectionRealTimeStatus_t status;
    if (m_sockets->GetConnectionRealTimeStatus(conn, &status, 0, nullptr) != k_EResultOK)
        return {};
    PeerLinkStats s;
    s.rttMs = status.m_nPing < 0 ? 0u : static_cast<uint32_t>(status.m_nPing);
    s.rttVarianceMs = 0u; // GNS does not surface RTT variance in the realtime status
    // m_flConnectionQualityLocal is the fraction of packets received; approximate loss as 1 - quality.
    const float quality = status.m_flConnectionQualityLocal;
    s.packetLoss = (quality >= 0.f && quality <= 1.f) ? (1.f - quality) : 0.f;
    s.reliableBytesInFlight =
        status.m_cbSentUnackedReliable < 0 ? 0u : static_cast<uint32_t>(status.m_cbSentUnackedReliable);
    return s;
}

void GnsNetwork::setBandwidthLimit(uint32_t incomingBps, uint32_t outgoingBps) {
    // GNS models send rate per-connection; cap the outgoing rate globally as the closest analogue to
    // ENet's host bandwidth limit. 0 = unlimited (leave GNS defaults).
    if (!m_sockets || outgoingBps == 0)
        return;
    ISteamNetworkingUtils* utils = SteamNetworkingUtils();
    if (!utils)
        return;
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, static_cast<int32_t>(outgoingBps));
    (void)incomingBps; // GNS does not expose a symmetric receive cap
}

void GnsNetwork::setPreHandshakeRateLimit(int /*maxAttempts*/, int /*windowMs*/) {
    // No-op: GNS drops unauthenticated connection floods internally before app-visible state exists.
}

// -------------------------------------------------------------------------
// Private helpers
// -------------------------------------------------------------------------

uint32_t GnsNetwork::connForPeer(uint32_t peerId) const {
    auto it = m_peerToConn.find(peerId);
    return it != m_peerToConn.end() ? it->second : 0u;
}

void GnsNetwork::closeAndErase(uint32_t conn, bool notify) {
    auto it = m_connToPeer.find(conn);
    if (it != m_connToPeer.end()) {
        const uint32_t peerId = it->second;
        if (notify && m_handler)
            m_handler->onDisconnect(peerId);
        m_peerToConn.erase(peerId);
        m_connToPeer.erase(it);
    }
    if (m_sockets)
        m_sockets->CloseConnection(conn, 0, nullptr, false);
    if (conn == m_clientConn)
        m_clientConn = 0;
}

} // namespace fl
