// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

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
};
