// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared INetwork test doubles. Kept out of mock_hal.h so HAL-only tests don't pull in
// platform/net. Naming convention (mirrors mock_hal.h): Null* = no-op base, Tracking* = records
// calls. Derive and override only what a test exercises.

#include "INetwork.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Every method a benign default: connections "succeed", queries return empty/disconnected,
// sends/broadcasts are dropped. Derive to add behaviour or recording.
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
    std::map<uint32_t, std::string> peerAddresses; // configure per-test
    std::vector<uint32_t> disconnectedPeers;       // tracks disconnectPeer calls
    int disconnectCount{0};                        // tracks the client-side disconnect() calls
    mutable std::string addrBuf;                   // backing store for getPeerAddress

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
};
