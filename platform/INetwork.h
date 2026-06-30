// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace fl {

// Implement this interface and register it with INetwork::setEventHandler.
// The backend calls these methods from INetwork::service() as events arrive.
// Threading: callbacks are invoked from whichever thread calls INetwork::service().
class INetworkEventHandler {
  public:
    virtual ~INetworkEventHandler() = default;

    virtual void onConnect(uint32_t peerId) = 0;
    virtual void onDisconnect(uint32_t peerId) = 0;
    virtual void onReceive(uint32_t peerId, const void* data, std::size_t size) = 0;
};

enum class PeerState : uint8_t { Connecting, Connected, Disconnecting, Disconnected };

// Per-peer link-quality snapshot, surfaced from the transport backend for congestion control (#518).
// All fields are 0 when the peer is not connected or the backend does not track the metric (e.g. mock
// implementations). packetLoss is a fraction in [0, 1]; rtt fields are milliseconds.
struct PeerLinkStats {
    uint32_t rttMs{0};                 // mean round-trip time
    uint32_t rttVarianceMs{0};         // round-trip time variance
    float packetLoss{0.f};             // mean reliable-channel loss fraction, 0..1
    uint32_t reliableBytesInFlight{0}; // reliable data sent but not yet acknowledged
};

// Threading: all methods must be called from the same thread (typically the main
// thread). service() is called once per frame from the game loop.
class INetwork {
  public:
    virtual ~INetwork() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    virtual void setEventHandler(INetworkEventHandler* handler) = 0;

    // --- Server side ---

    // Creates a host listening on the given address:port with up to maxClients peers.
    // address: IPv4 dotted-decimal ("0.0.0.0" for any), IPv6 literal ("::1"),
    //          "::" for dual-stack any, or nullptr for platform default.
    // Use "127.0.0.1" / "::1" for single-player / localhost-only servers.
    virtual bool bind(const char* address, uint16_t port, int maxClients) = 0;

    // --- Client side ---

    // Initiates an outbound connection to host:port. Returns true if the
    // connection attempt was queued successfully; the handshake completes
    // asynchronously. onConnect() fires via service() once the peer responds.
    // Returns false if the host could not be created or the peer slot is full.
    virtual bool connect(const char* host, uint16_t port) = 0;
    virtual void disconnect() = 0;

    // --- Data transfer ---

    // peerId is ignored on a pure client; set reliable=true for sequenced delivery.
    virtual bool send(uint32_t peerId, const void* data, std::size_t size, bool reliable) = 0;

    // Sends data to all currently connected peers.
    virtual void broadcast(const void* data, std::size_t size, bool reliable) = 0;

    // --- Frame pump ---

    // Drives the underlying I/O library; calls the event handler for each queued
    // event. Must be called once per frame. Pass timeoutMs=0 for non-blocking.
    virtual void service(int timeoutMs = 0) = 0;

    // --- Peer info ---

    virtual int getPeerCount() const = 0;
    virtual PeerState getPeerState(uint32_t peerId) const = 0;

    // Returns "ip:port" string for the given peer, or nullptr if not connected.
    // Valid until the next call on this interface.
    virtual const char* getPeerAddress(uint32_t peerId) const = 0;

    // Initiates a graceful disconnect of a single peer. No-op if peerId is not connected.
    virtual void disconnectPeer(uint32_t peerId) = 0;

    // Returns a human-readable description of the last error, or nullptr if none.
    // Valid until the next call on this interface.
    virtual const char* getLastError() const = 0;

    // Returns ENet's rolling round-trip time estimate for the given peer in milliseconds.
    // Returns 0 if peerId is out of range, the peer is not connected, or the backend
    // does not track RTT (e.g. mock implementations).
    virtual uint32_t getPeerRtt(uint32_t peerId) const = 0;

    // Returns per-peer link-quality stats (RTT, RTT variance, packet loss, reliable bytes in flight)
    // for congestion control (#518). All-zero when peerId is out of range, the peer is not connected,
    // or the backend does not track link quality (mocks). Superset of getPeerRtt server-side.
    virtual PeerLinkStats getPeerLinkStats(uint32_t peerId) const = 0;
};

} // namespace fl
