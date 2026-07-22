// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for parseLobbyServerList (#143): the tolerant, bounded JSON scanner for a lobby's
// GET /v1/servers response.

#include "net/LobbyListClient.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

TEST_CASE("parseLobbyServerList: parses a well-formed list", "[lobby_list]") {
    const std::string json = R"([
      {"name":"Alpha","host":"1.2.3.4","port":4778,"mode":"builtin:tdm","mission":"fjord",
       "players":3,"max_players":16,"passworded":true},
      {"name":"Bravo","host":"example.com","port":5000,"players":0,"max_players":8}
    ])";
    auto rows = parseLobbyServerList(json);
    REQUIRE(rows.size() == 2u);
    CHECK(rows[0].name == "Alpha");
    CHECK(rows[0].host == "1.2.3.4");
    CHECK(rows[0].port == 4778);
    CHECK(rows[0].mode == "builtin:tdm");
    CHECK(rows[0].mission == "fjord");
    CHECK(rows[0].players == 3);
    CHECK(rows[0].maxPlayers == 16);
    CHECK(rows[0].passworded);
    CHECK(rows[1].name == "Bravo");
    CHECK(rows[1].host == "example.com");
    CHECK(rows[1].port == 5000);
    CHECK_FALSE(rows[1].passworded);
}

TEST_CASE("parseLobbyServerList: drops rows with no host or zero port", "[lobby_list]") {
    const std::string json = R"([
      {"name":"NoHost","port":4778},
      {"name":"NoPort","host":"1.2.3.4"},
      {"name":"Good","host":"1.2.3.4","port":4778}
    ])";
    auto rows = parseLobbyServerList(json);
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].name == "Good");
}

TEST_CASE("parseLobbyServerList: ignores unknown keys and nested structures", "[lobby_list]") {
    const std::string json = R"([{"host":"h","port":1,"extra":{"a":1,"b":[1,2,3]},"tags":["x","y"],"name":"N"}])";
    auto rows = parseLobbyServerList(json);
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].host == "h");
    CHECK(rows[0].name == "N");
}

TEST_CASE("parseLobbyServerList: tolerates malformed / truncated input", "[lobby_list]") {
    CHECK(parseLobbyServerList("").empty());
    CHECK(parseLobbyServerList("not json").empty());
    CHECK(parseLobbyServerList("{}").empty()); // not an array
    // Truncated before the port is known: the incomplete row is dropped (no port).
    CHECK(parseLobbyServerList(R"([{"host":"h")").empty());
    // A valid row followed by garbage keeps the valid row.
    auto rows = parseLobbyServerList(R"([{"host":"h","port":1,"name":"A"}, garbage )");
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].name == "A");
}

TEST_CASE("parseLobbyServerList: escaped strings decode", "[lobby_list]") {
    auto rows = parseLobbyServerList(R"([{"host":"h","port":1,"name":"a\"b\\c\nd"}])");
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].name == "a\"b\\c\nd");
}

TEST_CASE("parseLobbyServerList: caps the row count", "[lobby_list]") {
    std::string json = "[";
    for (std::size_t i = 0; i < kMaxLobbyServers + 50; ++i) {
        if (i)
            json += ",";
        json += R"({"host":"h","port":1})";
    }
    json += "]";
    auto rows = parseLobbyServerList(json);
    CHECK(rows.size() <= kMaxLobbyServers);
}

TEST_CASE("parseLobbyServerList: caps per-string length", "[lobby_list]") {
    std::string big(kMaxLobbyStringBytes + 500, 'x');
    auto rows = parseLobbyServerList(R"([{"host":"h","port":1,"name":")" + big + R"("}])");
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].name.size() <= kMaxLobbyStringBytes);
}
