// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for ServerBrowserModel (#143): merging LAN discovery, lobby listings, and query results
// into a sorted, deduplicated row list.

#include "net/GameProtocol.h"
#include "net/ServerBrowserModel.h"
#include <algorithm>

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

// ---------------------------------------------------------------------------
// #1074: the browser shows a server's build, and flags a mismatch
// ---------------------------------------------------------------------------

TEST_CASE("ServerBrowserModel: a LAN row carries the advertised build and flags a mismatch (#1074)",
          "[server_browser]") {
    ServerBrowserModel model;
    model.setClientBuildVersion("0.4.1");

    DiscoveryListener::ServerInfo older{};
    older.address = "10.0.0.5";
    older.beacon.gamePort = 4778;
    std::snprintf(older.beacon.name, sizeof(older.beacon.name), "%s", "Old Server");
    std::snprintf(older.beacon.build, sizeof(older.beacon.build), "%s", "0.3.11");

    DiscoveryListener::ServerInfo same{};
    same.address = "10.0.0.6";
    same.beacon.gamePort = 4778;
    std::snprintf(same.beacon.name, sizeof(same.beacon.name), "%s", "Current Server");
    std::snprintf(same.beacon.build, sizeof(same.beacon.build), "%s", "0.4.1");

    // A server that predates the field advertises nothing — not a mismatch.
    DiscoveryListener::ServerInfo silent{};
    silent.address = "10.0.0.7";
    silent.beacon.gamePort = 4778;
    std::snprintf(silent.beacon.name, sizeof(silent.beacon.name), "%s", "Silent Server");

    model.rebuild({older, same, silent}, {}, {});
    REQUIRE(model.rows().size() == 3u);

    // find_if + REQUIRE rather than a loop with a trailing FAIL: Catch2's FAIL throws, so MSVC reads
    // any return after it as unreachable code and /WX turns that warning into a build failure, while
    // GCC and Clang want a return on every path. This shape satisfies both.
    auto rowFor = [&](const std::string& host) -> const BrowserRow& {
        const auto it =
            std::find_if(model.rows().begin(), model.rows().end(), [&](const BrowserRow& r) { return r.host == host; });
        REQUIRE(it != model.rows().end());
        return *it;
    };

    CHECK(rowFor("10.0.0.5").build == "0.3.11");
    CHECK(rowFor("10.0.0.5").buildMismatch);
    CHECK(rowFor("10.0.0.6").build == "0.4.1");
    CHECK_FALSE(rowFor("10.0.0.6").buildMismatch);
    CHECK(rowFor("10.0.0.7").build.empty());
    CHECK_FALSE(rowFor("10.0.0.7").buildMismatch);
}

TEST_CASE("ServerBrowserModel: a live query corrects the advertised build (#1074)", "[server_browser]") {
    ServerBrowserModel model;
    model.setClientBuildVersion("0.4.1");

    DiscoveryListener::ServerInfo lan{};
    lan.address = "10.0.0.5";
    lan.beacon.gamePort = 4778;
    std::snprintf(lan.beacon.name, sizeof(lan.beacon.name), "%s", "Upgraded");
    std::snprintf(lan.beacon.build, sizeof(lan.beacon.build), "%s", "0.3.11"); // stale beacon

    ServerQueryClient::Result q{};
    q.address = "10.0.0.5";
    q.info.gamePort = 4778;
    q.rttMs = 12.f;
    std::snprintf(q.info.build, sizeof(q.info.build), "%s", "0.4.1"); // the server has since restarted

    model.rebuild({lan}, {}, {q});
    REQUIRE(model.rows().size() == 1u);
    CHECK(model.rows()[0].build == "0.4.1");
    CHECK_FALSE(model.rows()[0].buildMismatch);
}

TEST_CASE("ServerBrowserModel: a client that does not know its own build judges nobody (#1074)", "[server_browser]") {
    // setClientBuildVersion is never called, so the model has nothing to compare against and must not
    // flag every server as mismatched.
    ServerBrowserModel model;

    DiscoveryListener::ServerInfo lan{};
    lan.address = "10.0.0.5";
    lan.beacon.gamePort = 4778;
    std::snprintf(lan.beacon.build, sizeof(lan.beacon.build), "%s", "0.3.11");

    model.rebuild({lan}, {}, {});
    REQUIRE(model.rows().size() == 1u);
    CHECK(model.rows()[0].build == "0.3.11");
    CHECK_FALSE(model.rows()[0].buildMismatch);
}
