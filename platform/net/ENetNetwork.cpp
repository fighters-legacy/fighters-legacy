// SPDX-License-Identifier: GPL-3.0-or-later
#include "ENetNetwork.h"
#include <cstring>
#include <enet6/enet.h>

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------

ENetNetwork::~ENetNetwork() {
    if (m_host || m_initialized)
        shutdown();
}

bool ENetNetwork::init() {
    if (m_initialized)
        return true; // idempotent — enet_initialize() is not ref-counted
    if (enet_initialize() != 0) {
        m_lastError = "enet_initialize() failed";
        return false;
    }
    m_initialized = true;
    return true;
}

void ENetNetwork::shutdown() {
    if (m_host) {
        drainPeers();
        enet_host_destroy(m_host);
        m_host = nullptr;
    }
    if (m_initialized) {
        enet_deinitialize();
        m_initialized = false;
    }
    m_isServer = false;
    m_handler = nullptr;
}

void ENetNetwork::setEventHandler(INetworkEventHandler* handler) {
    m_handler = handler;
}

// -------------------------------------------------------------------------
// Server / client setup
// -------------------------------------------------------------------------

bool ENetNetwork::bind(const char* address, uint16_t port, int maxClients) {
    if (m_host) {
        m_lastError = "already bound or connected";
        return false;
    }
    ENetAddress addr{};
    addr.port = port;
    if (!address || address[0] == '\0' || std::strcmp(address, "0.0.0.0") == 0) {
        addr.type = ENET_ADDRESS_TYPE_IPV4;
        // v4 union zeroed = INADDR_ANY
    } else if (std::strcmp(address, "::") == 0) {
        addr.type = ENET_ADDRESS_TYPE_IPV6;
        // v6 union zeroed = IN6ADDR_ANY; dual-stack on Linux, IPv6-only on Windows
    } else {
        if (enet_address_set_host_ip(&addr, address) != 0) {
            m_lastError = "enet_address_set_host_ip() failed — invalid bind address";
            return false;
        }
    }
    m_host = enet_host_create(addr.type, &addr, static_cast<size_t>(maxClients), kChannelCount, 0, 0);
    if (!m_host) {
        m_lastError = "enet_host_create() failed";
        return false;
    }
    // CRC32 checksums catch corruption stronger than UDP's built-in checksum.
    m_host->checksum = enet_crc32;
    // Adaptive range coder reduces bandwidth ~20-40% for game state traffic.
    enet_host_compress_with_range_coder(m_host);
    m_isServer = true;
    return true;
}

bool ENetNetwork::connect(const char* host, uint16_t port) {
    if (m_host) {
        m_lastError = "already bound or connected";
        return false;
    }
    // Resolve remote address first so we know the address type (IPv4 vs IPv6).
    ENetAddress addr{};
    if (enet_address_set_host(&addr, ENET_ADDRESS_TYPE_ANY, host) != 0) {
        m_lastError = "enet_address_set_host() failed — could not resolve host";
        return false;
    }
    addr.port = port;
    // Client host: no local address, 1 outbound peer slot, type matches remote.
    m_host = enet_host_create(addr.type, nullptr, 1, kChannelCount, 0, 0);
    if (!m_host) {
        m_lastError = "enet_host_create() failed";
        return false;
    }
    m_host->checksum = enet_crc32;
    enet_host_compress_with_range_coder(m_host);

    ENetPeer* peer = enet_host_connect(m_host, &addr, kChannelCount, 0);
    if (!peer) {
        enet_host_destroy(m_host);
        m_host = nullptr;
        m_lastError = "enet_host_connect() failed — no peer slots";
        return false;
    }
    // Handshake completes asynchronously; onConnect() fires via service().
    m_isServer = false;
    return true;
}

void ENetNetwork::disconnect() {
    if (!m_host)
        return;
    drainPeers();
    enet_host_destroy(m_host);
    m_host = nullptr;
    m_isServer = false;
}

// -------------------------------------------------------------------------
// Data transfer
// -------------------------------------------------------------------------

bool ENetNetwork::send(uint32_t peerId, const void* data, std::size_t size, bool reliable) {
    if (!m_host) {
        m_lastError = "not connected";
        return false;
    }
    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    enet_uint8 channel = reliable ? static_cast<enet_uint8>(kChReliable) : static_cast<enet_uint8>(kChUnreliable);
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    if (!pkt) {
        m_lastError = "enet_packet_create() failed";
        return false;
    }
    if (m_isServer) {
        if (peerId >= m_host->peerCount) {
            enet_packet_destroy(pkt);
            m_lastError = "invalid peerId";
            return false;
        }
        ENetPeer* peer = &m_host->peers[peerId];
        if (peer->state != ENET_PEER_STATE_CONNECTED) {
            enet_packet_destroy(pkt);
            m_lastError = "peer not connected";
            return false;
        }
        enet_peer_send(peer, channel, pkt);
    } else {
        // Client has exactly one peer; peerId is ignored.
        enet_host_broadcast(m_host, channel, pkt);
    }
    return true;
}

void ENetNetwork::broadcast(const void* data, std::size_t size, bool reliable) {
    if (!m_host) {
        m_lastError = "not connected";
        return;
    }
    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    enet_uint8 channel = reliable ? static_cast<enet_uint8>(kChReliable) : static_cast<enet_uint8>(kChUnreliable);
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    if (!pkt) {
        m_lastError = "enet_packet_create() failed";
        return;
    }
    enet_host_broadcast(m_host, channel, pkt);
}

// -------------------------------------------------------------------------
// Frame pump
// -------------------------------------------------------------------------

void ENetNetwork::service(int timeoutMs) {
    if (!m_host)
        return;
    ENetEvent ev;
    int r;
    while ((r = enet_host_service(m_host, &ev, static_cast<enet_uint32>(timeoutMs))) > 0) {
        timeoutMs = 0; // only block on the first iteration
        uint32_t id = static_cast<uint32_t>(ev.peer - m_host->peers);
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            // Override ENet's default 30 s max timeout. 500 ms min / 5 s max
            // detects dead peers far faster without being brittle on jitter.
            enet_peer_timeout(ev.peer, 32, 500, 5000);
            ev.peer->data = nullptr; // reserved for Phase 2 per-peer session state
            if (m_handler)
                m_handler->onConnect(id);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
        case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
            if (m_handler)
                m_handler->onDisconnect(id);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (m_handler)
                m_handler->onReceive(id, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet); // always destroy, even if no handler
            break;
        default:
            break;
        }
    }
    if (r < 0)
        m_lastError = "enet_host_service() error";
}

// -------------------------------------------------------------------------
// Peer info
// -------------------------------------------------------------------------

int ENetNetwork::getPeerCount() const {
    if (!m_host)
        return 0;
    int count = 0;
    for (size_t i = 0; i < m_host->peerCount; ++i) {
        if (m_host->peers[i].state != ENET_PEER_STATE_DISCONNECTED)
            ++count;
    }
    return count;
}

PeerState ENetNetwork::getPeerState(uint32_t peerId) const {
    if (!m_host || peerId >= m_host->peerCount)
        return PeerState::Disconnected;
    switch (m_host->peers[peerId].state) {
    case ENET_PEER_STATE_CONNECTING:
    case ENET_PEER_STATE_ACKNOWLEDGING_CONNECT:
    case ENET_PEER_STATE_CONNECTION_PENDING:
    case ENET_PEER_STATE_CONNECTION_SUCCEEDED:
        return PeerState::Connecting;
    case ENET_PEER_STATE_CONNECTED:
        return PeerState::Connected;
    case ENET_PEER_STATE_DISCONNECT_LATER:
    case ENET_PEER_STATE_DISCONNECTING:
    case ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT:
        return PeerState::Disconnecting;
    case ENET_PEER_STATE_DISCONNECTED:
    case ENET_PEER_STATE_ZOMBIE:
    default:
        return PeerState::Disconnected;
    }
}

const char* ENetNetwork::getPeerAddress(uint32_t peerId) const {
    if (!m_host || peerId >= m_host->peerCount)
        return nullptr;
    ENetPeer* peer = &m_host->peers[peerId];
    if (peer->state == ENET_PEER_STATE_DISCONNECTED)
        return nullptr;
    char ip[ENET_ADDRESS_MAX_LENGTH + 1];
    enet_address_get_host_ip(&peer->address, ip, sizeof(ip));
    if (peer->address.type == ENET_ADDRESS_TYPE_IPV6)
        m_peerAddressBuf = std::string("[") + ip + "]:" + std::to_string(peer->address.port);
    else
        m_peerAddressBuf = std::string(ip) + ":" + std::to_string(peer->address.port);
    return m_peerAddressBuf.c_str();
}

void ENetNetwork::disconnectPeer(uint32_t peerId) {
    if (!m_host || peerId >= m_host->peerCount)
        return;
    ENetPeer* peer = &m_host->peers[peerId];
    if (peer->state != ENET_PEER_STATE_DISCONNECTED)
        enet_peer_disconnect(peer, 0);
}

const char* ENetNetwork::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

// -------------------------------------------------------------------------
// Private helpers
// -------------------------------------------------------------------------

void ENetNetwork::drainPeers() {
    for (size_t i = 0; i < m_host->peerCount; ++i) {
        ENetPeer* peer = &m_host->peers[i];
        if (peer->state != ENET_PEER_STATE_DISCONNECTED)
            enet_peer_disconnect(peer, 0);
    }
    // Drain up to 100 ms for DISCONNECT events; destroy any stray packets.
    ENetEvent ev;
    while (enet_host_service(m_host, &ev, 100) > 0) {
        if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            enet_packet_destroy(ev.packet);
    }
}
