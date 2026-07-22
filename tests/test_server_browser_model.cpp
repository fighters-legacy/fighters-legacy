// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for ServerBrowserModel (#143): merging LAN discovery, lobby listings, and query results
// into a sorted, deduplicated row list.

#include "net/GameProtocol.h"
#include "net/ServerBrowserModel.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

namespace {
DiscoveryListener::ServerInfo lanServer(const char* addr, const char* name, uint16_t port, uint8_t players,
                                        uint16_t queryPort = 4779, uint16_t proto = kProtocolVersion) {
    DiscoveryListener::ServerInfo s;
    s.address = addr;
    std::snprintf(s.beacon.name, sizeof(s.beacon.name), "%s", name);
    s.beacon.gamePort = port;
    s.beacon.playerCount = players;
    s.beacon.maxPlayers = 16;
    s.beacon.queryPort = queryPort;
    s.beacon.protocolVersion = proto;
    return s;
}
LobbyServer lobbyServer(const char* host, const char* name, uint16_t port, int players) {
    LobbyServer s;
    s.host = host;
    s.name = name;
    s.port = port;
    s.players = players;
    s.maxPlayers = 16;
    return s;
}
ServerQueryClient::Result queryResult(const char* addr, uint16_t gamePort, uint8_t players, float ping,
                                      uint16_t proto = kProtocolVersion) {
    ServerQueryClient::Result r;
    r.address = addr;
    r.queryPort = 4779;
    r.rttMs = ping;
    r.info.gamePort = gamePort;
    r.info.playerCount = players;
    r.info.maxPlayers = 16;
    r.info.protocolVersion = proto;
    std::snprintf(r.info.mission, sizeof(r.info.mission), "canyon");
    return r;
}
} // namespace

TEST_CASE("ServerBrowserModel: merges LAN and lobby, dedup LAN-wins (#143)", "[browser_model]") {
    ServerBrowserModel m;
    std::vector<DiscoveryListener::ServerInfo> lan{lanServer("1.2.3.4", "LanHost", 4778, 2)};
    std::vector<LobbyServer> lobby{
        lobbyServer("1.2.3.4", "LobbyDup", 4778, 9), // same host:port as LAN → dropped
        lobbyServer("remote.example", "Remote", 5000, 1),
    };
    m.rebuild(lan, lobby, {});
    REQUIRE(m.rows().size() == 2u);
    // The LAN row wins the dedup.
    const BrowserRow* lanRow = nullptr;
    const BrowserRow* remote = nullptr;
    for (const auto& r : m.rows()) {
        if (r.host == "1.2.3.4")
            lanRow = &r;
        if (r.host == "remote.example")
            remote = &r;
    }
    REQUIRE(lanRow);
    REQUIRE(remote);
    CHECK(lanRow->name == "LanHost");
    CHECK(lanRow->source == BrowserSource::Lan);
    CHECK(remote->source == BrowserSource::Lobby);
}

TEST_CASE("ServerBrowserModel: a query result attaches ping + fresh counts (#143)", "[browser_model]") {
    ServerBrowserModel m;
    std::vector<DiscoveryListener::ServerInfo> lan{lanServer("1.2.3.4", "Host", 4778, 2)};
    std::vector<ServerQueryClient::Result> q{queryResult("1.2.3.4", 4778, 7, 42.5f)};
    m.rebuild(lan, {}, q);
    REQUIRE(m.rows().size() == 1u);
    const BrowserRow& r = m.rows()[0];
    CHECK(r.hasPing);
    CHECK(r.pingMs == 42.5f);
    CHECK(r.players == 7); // fresher than the beacon's 2
    CHECK(r.mission == "canyon");
}

TEST_CASE("ServerBrowserModel: protocol mismatch flagged + sunk in the sort (#143)", "[browser_model]") {
    ServerBrowserModel m;
    std::vector<DiscoveryListener::ServerInfo> lan{
        lanServer("1.1.1.1", "Bad", 4778, 5, 4779, static_cast<uint16_t>(kProtocolVersion + 1)),
        lanServer("2.2.2.2", "Good", 4778, 1),
    };
    m.rebuild(lan, {}, {});
    REQUIRE(m.rows().size() == 2u);
    // Good (compatible) sorts above Bad (mismatch) despite fewer players.
    CHECK(m.rows()[0].name == "Good");
    CHECK_FALSE(m.rows()[0].protocolMismatch);
    CHECK(m.rows()[1].name == "Bad");
    CHECK(m.rows()[1].protocolMismatch);
}

TEST_CASE("ServerBrowserModel: joinable rows sort by descending players (#143)", "[browser_model]") {
    ServerBrowserModel m;
    std::vector<LobbyServer> lobby{
        lobbyServer("a", "A", 1, 1),
        lobbyServer("b", "B", 1, 9),
        lobbyServer("c", "C", 1, 4),
    };
    m.rebuild({}, lobby, {});
    REQUIRE(m.rows().size() == 3u);
    CHECK(m.rows()[0].name == "B"); // 9
    CHECK(m.rows()[1].name == "C"); // 4
    CHECK(m.rows()[2].name == "A"); // 1
}
