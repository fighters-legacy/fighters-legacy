// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared INetwork test doubles. Kept out of mock_hal.h so HAL-only tests don't pull in
// platform/net. Naming convention (mirrors mock_hal.h): Null* = no-op base, Tracking* = records
// calls. Derive and override only what a test exercises.

#include "INetwork.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

// Every method a benign default: connections "succeed", queries return empty/disconnected,
// sends/broadcasts are dropped. Derive to add behaviour or recording.
namespace fl {

struct NullNetwork : INetwork {
    bool init() override {
        return true;
    }
    void shutdown() override {}
    void setEventHandler(INetworkEventHandler*) override {}
    bool bind(const char*, uint16_t, int) override {
        return true;
    }
    bool connect(const char*, uint16_t) override {
        return true;
    }
    void disconnect() override {}
    bool send(uint32_t, const void*, std::size_t, bool) override {
        return true;
    }
    void broadcast(const void*, std::size_t, bool) override {}
    void service(int) override {}
    int getPeerCount() const override {
        return 0;
    }
    PeerState getPeerState(uint32_t) const override {
        return PeerState::Disconnected;
    }
    const char* getPeerAddress(uint32_t) const override {
        return nullptr;
    }
    void disconnectPeer(uint32_t) override {}
    const char* getLastError() const override {
        return nullptr;
    }
    uint32_t getPeerRtt(uint32_t) const override {
        return 0u;
    }
    PeerLinkStats getPeerLinkStats(uint32_t) const override {
        return {};
    }
};

// Records emitted packets, disconnect calls, and the reliability flag; resolves per-peer addresses
// from a configurable map. Used by server/handler tests that assert on what was sent.
struct TrackingNetwork : NullNetwork {
    std::vector<std::vector<uint8_t>> broadcasts;
    std::vector<std::vector<uint8_t>> sends;
    // All unicast sends recorded with their destination peerId; used by interest-management
    // tests to assert on per-peer snapshot content without touching the existing sends list.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> perPeerSends;
    bool sendReliable{false};
    uint8_t lastChannel{0xFFu};                      // channel of the last sendChannel() call; 0xFF = never called
    std::map<uint32_t, std::string> peerAddresses;   // configure per-test
    std::map<uint32_t, PeerLinkStats> peerLinkStats; // configure per-test (congestion tests)
    std::vector<uint32_t> disconnectedPeers;         // tracks disconnectPeer calls
    int disconnectCount{0};                          // tracks the client-side disconnect() calls
    mutable std::string addrBuf;                     // backing store for getPeerAddress

    void disconnect() override {
        ++disconnectCount;
    }
    bool send(uint32_t peerId, const void* data, std::size_t size, bool reliable) override {
        std::vector<uint8_t> pkt{static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size};
        sends.push_back(pkt);
        perPeerSends.emplace_back(peerId, std::move(pkt));
        sendReliable = reliable;
        return true;
    }
    // Records the channel then forwards, so a test can assert voice actually rode kNetChVoice (#532)
    // without every existing assertion on `sends` having to change.
    bool sendChannel(uint32_t peerId, const void* data, std::size_t size, bool reliable, uint8_t channel) override {
        lastChannel = channel;
        return send(peerId, data, size, reliable);
    }
    void broadcast(const void* data, std::size_t size, bool) override {
        broadcasts.push_back({static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size});
    }
    void disconnectPeer(uint32_t peerId) override {
        disconnectedPeers.push_back(peerId);
    }
    const char* getPeerAddress(uint32_t peerId) const override {
        auto it = peerAddresses.find(peerId);
        if (it == peerAddresses.end())
            return nullptr;
        addrBuf = it->second;
        return addrBuf.c_str();
    }
    PeerLinkStats getPeerLinkStats(uint32_t peerId) const override {
        auto it = peerLinkStats.find(peerId);
        return it == peerLinkStats.end() ? PeerLinkStats{} : it->second;
    }
};

// ---------------------------------------------------------------------------
// Real-loopback scaffolding (#1276)
// ---------------------------------------------------------------------------
//
// The ENet and GNS backend suites both drive two REAL sockets against each other and record what
// came out. They had identical copies of the recorder and the service pump; test_gns_network.cpp's
// own header comment says it "mirrors test_network.cpp's structure", which is exactly the state
// this header exists to end.
//
// This is not an INetwork double -- it is the harness a real backend is tested THROUGH -- but it
// belongs with the doubles: same interface, same consumers, same stdlib-only include set.

// One recorded callback from a backend under test.
struct Event {
    enum class Type { Connect, Disconnect, Receive };
    Type type;
    uint32_t peerId{0};
    std::vector<uint8_t> data; // populated for Receive events
};

struct EventSink : INetworkEventHandler {
    std::vector<Event> events;

    void onConnect(uint32_t peerId) override {
        events.push_back({Event::Type::Connect, peerId, {}});
    }
    void onDisconnect(uint32_t peerId) override {
        events.push_back({Event::Type::Disconnect, peerId, {}});
    }
    void onReceive(uint32_t peerId, const void* data, std::size_t size) override {
        Event e;
        e.type = Event::Type::Receive;
        e.peerId = peerId;
        e.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        events.push_back(std::move(e));
    }

    [[nodiscard]] int countType(Event::Type t) const {
        int n = 0;
        for (const auto& ev : events)
            if (ev.type == t)
                ++n;
        return n;
    }
};

// Service both ends `iters` times, giving each `msPerIter` of budget.
//
// ⚠ msPerIter has NO DEFAULT, deliberately. The two copies defaulted it to 10 and 15 -- a tuning
// difference, not a semantic one, but a shared default would have silently retimed whichever suite
// did not match it. Stating the budget at the call site is how the two suites stay honestly
// different where they are different.
inline void pump(INetwork& server, INetwork& client, int iters, int msPerIter) {
    for (int i = 0; i < iters; ++i) {
        server.service(msPerIter);
        client.service(msPerIter);
    }
}

// The same for one server and several clients (the multi-client ENet cases).
inline void pumpN(INetwork& server, std::initializer_list<INetwork*> clients, int iters, int msPerIter) {
    for (int i = 0; i < iters; ++i) {
        server.service(msPerIter);
        for (INetwork* c : clients)
            c->service(msPerIter);
    }
}

} // namespace fl
