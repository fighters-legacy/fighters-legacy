// SPDX-License-Identifier: GPL-3.0-or-later
//
// LobbyListClient's request lifecycle (#1145). test_lobby_list_parser.cpp covers the JSON parser;
// this file covers the object around it — URL construction, the in-flight guard, body accumulation
// across chunks, and every way a fetch can fail.
//
// The failure modes matter because this runs against a REMOTE service the project does not control:
// a lobby that is down, slow, redirected, or returning a 500 with an HTML error page must leave the
// server browser with a clear "that failed" rather than a half-parsed list or a stuck spinner.

#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "mock_log.h"
#include "net/LobbyListClient.h"

#include "mock_http.h"

#include <string>
#include <vector>

using namespace fl;

namespace {

constexpr const char* kEndpoint = "https://lobby.example.com/v1/servers";

constexpr const char* kTwoServers = R"([
  {"name":"Alpha","host":"1.2.3.4","port":4778,"players":3,"max_players":16,"mode":"dogfight"},
  {"name":"Bravo","address":"5.6.7.8","port":4779,"players":0,"maxPlayers":32,"passworded":true}
])";

} // namespace

TEST_CASE("LobbyListClient: a successful fetch populates the server list (#1145)", "[lobby][net]") {
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, kTwoServers, 200);

    REQUIRE(client.refresh("https://lobby.example.com"));
    CHECK(client.inFlight());
    http.service();

    CHECK_FALSE(client.inFlight());
    CHECK_FALSE(client.lastFetchFailed());
    REQUIRE(client.servers().size() == 2);
    CHECK(client.servers()[0].name == "Alpha");
    CHECK(client.servers()[0].host == "1.2.3.4");
    CHECK(client.servers()[0].port == 4778);
    CHECK(client.servers()[1].host == "5.6.7.8"); // the `address` spelling is accepted too
    CHECK(client.servers()[1].passworded);
}

TEST_CASE("LobbyListClient: the endpoint path is appended exactly once (#1145)", "[lobby][net]") {
    // A trailing slash in an operator-configured URL is the most likely way to get "//v1/servers",
    // which some lobbies 404 on.
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, "[]", 200);

    REQUIRE(client.refresh("https://lobby.example.com/"));
    REQUIRE_FALSE(http.requestedUrls.empty());
    CHECK(http.requestedUrls.back() == kEndpoint);
}

TEST_CASE("LobbyListClient: an empty URL is not a request (#1145)", "[lobby][net]") {
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);

    CHECK_FALSE(client.refresh(""));
    CHECK_FALSE(client.inFlight());
    CHECK(http.requestedUrls.empty());
}

TEST_CASE("LobbyListClient: a second refresh while one is in flight is refused (#1145)", "[lobby][net]") {
    // Otherwise a player mashing the refresh button queues N identical fetches and the last one to
    // land wins, which is neither what they asked for nor kind to the lobby.
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, kTwoServers, 200);

    REQUIRE(client.refresh("https://lobby.example.com"));
    CHECK_FALSE(client.refresh("https://lobby.example.com"));
    CHECK(http.requestedUrls.size() == 1);

    http.service();
    CHECK(client.refresh("https://lobby.example.com")); // allowed again once it has landed
}

TEST_CASE("LobbyListClient: a transport error is reported and leaves the list alone (#1145)", "[lobby][net]") {
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);

    // First a good fetch, so there is a previous list to protect.
    http.setResponse(kEndpoint, kTwoServers, 200);
    REQUIRE(client.refresh("https://lobby.example.com"));
    http.service();
    REQUIRE(client.servers().size() == 2);

    http.responses.erase(kEndpoint); // no canned response: the mock answers with a transport error
    REQUIRE(client.refresh("https://lobby.example.com"));
    http.service();

    CHECK(client.lastFetchFailed());
    CHECK_FALSE(client.inFlight());
    CHECK(client.servers().size() == 2); // the stale-but-real list survives a failed refresh
    CHECK_FALSE(log.messages(LogLevel::Warn).empty());
}

TEST_CASE("LobbyListClient: a non-2xx response is a failure even with a body (#1145)", "[lobby][net]") {
    // A lobby behind a proxy returns 500 with an HTML error page. Parsing that as a server list
    // would yield an empty list and look like "no servers online" rather than "the lobby is down".
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, "<html>Internal Server Error</html>", 500);

    REQUIRE(client.refresh("https://lobby.example.com"));
    http.service();

    CHECK(client.lastFetchFailed());
    CHECK(client.servers().empty());
}

TEST_CASE("LobbyListClient: a 3xx redirect body is not treated as a list (#1145)", "[lobby][net]") {
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, "[]", 302);

    REQUIRE(client.refresh("https://lobby.example.com"));
    http.service();
    CHECK(client.lastFetchFailed());
}

TEST_CASE("LobbyListClient: data for a stale request id is ignored (#1145)", "[lobby][net]") {
    // A cancelled or superseded request can still deliver a chunk; letting it into the buffer would
    // splice two responses together.
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, kTwoServers, 200);
    REQUIRE(client.refresh("https://lobby.example.com"));

    const std::string junk = "GARBAGE";
    client.onHttpData(999999, junk.data(), junk.size()); // not our id
    http.service();

    REQUIRE(client.servers().size() == 2); // the real body parsed cleanly
}

TEST_CASE("LobbyListClient: completion for a stale request id is ignored (#1145)", "[lobby][net]") {
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, kTwoServers, 200);
    REQUIRE(client.refresh("https://lobby.example.com"));

    client.onHttpComplete(999999, HttpStatus::Error, 500, "not ours");
    CHECK(client.inFlight()); // our request is still outstanding
    CHECK_FALSE(client.lastFetchFailed());

    http.service();
    CHECK(client.servers().size() == 2);
}

TEST_CASE("LobbyListClient: a body split across chunks is accumulated before parsing (#1145)", "[lobby][net]") {
    // A JSON array arriving in 16-byte pieces must parse identically to one that arrives whole;
    // parsing per chunk would see truncated objects and drop every server.
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, kTwoServers, 200, /*chunkSize=*/16);

    REQUIRE(client.refresh("https://lobby.example.com"));
    http.service();
    CHECK_FALSE(client.lastFetchFailed());
    CHECK(client.servers().size() == 2);
}

TEST_CASE("LobbyListClient: an empty successful body yields an empty list, not a failure (#1145)", "[lobby][net]") {
    // A lobby with nobody hosting is a legitimate answer and must not read as an error.
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);
    http.setResponse(kEndpoint, "[]", 200);

    REQUIRE(client.refresh("https://lobby.example.com"));
    http.service();
    CHECK_FALSE(client.lastFetchFailed());
    CHECK(client.servers().empty());
}

TEST_CASE("LobbyListClient: a refresh after a failure clears the failure flag (#1145)", "[lobby][net]") {
    RecordingLogger log;
    TrackingHttpClient http;
    LobbyListClient client(http, log);

    // no canned response for the endpoint: the mock answers with a transport error
    REQUIRE(client.refresh("https://lobby.example.com"));
    http.service();
    REQUIRE(client.lastFetchFailed());

    http.setResponse(kEndpoint, kTwoServers, 200);
    REQUIRE(client.refresh("https://lobby.example.com"));
    CHECK_FALSE(client.lastFetchFailed()); // cleared at request time, not left stale on the UI
    http.service();
    CHECK_FALSE(client.lastFetchFailed());
}
