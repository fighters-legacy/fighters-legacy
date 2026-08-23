// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for LobbyRegistration (#143): the dedicated-server lobby heartbeat client, driven against
// the canned TrackingHttpClient + a manual clock (no sockets).

#include "net/LobbyRegistration.h"

#include "IClock.h"
#include "ILogger.h"
#include "mock_http.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

namespace {
struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

LobbyRegistrationConfig makeCfg() {
    LobbyRegistrationConfig c;
    c.lobbyUrl = "https://lobby.example/";
    c.name = "Test Server";
    c.gamePort = 4778;
    c.mode = "builtin:tdm";
    c.mission = "fjord";
    c.maxPlayers = 16;
    c.heartbeatS = 30;
    c.visibilityPublic = true;
    return c;
}
} // namespace

TEST_CASE("LobbyRegistration: first tick POSTs a JSON heartbeat to /v1/servers (#143)", "[lobby_reg]") {
    NullLog log;
    TrackingHttpClient http;
    http.setResponse("https://lobby.example/v1/servers", "", 200);
    ManualClock clock;
    LobbyRegistration reg(http, log);
    reg.setClock(clock);
    reg.configure(makeCfg());
    reg.setDynamic(3, "fjord");

    reg.tick();
    REQUIRE(http.requests.size() == 1u);
    const auto& req = http.requests.back();
    CHECK(req.method == HttpMethod::Post);
    CHECK(req.url == "https://lobby.example/v1/servers"); // trailing slash collapsed
    CHECK(req.contentType == "application/json");
    CHECK(req.body.find("\"name\":\"Test Server\"") != std::string::npos);
    CHECK(req.body.find("\"port\":4778") != std::string::npos);
    CHECK(req.body.find("\"players\":3") != std::string::npos);
    CHECK(req.body.find("\"max_players\":16") != std::string::npos);
    CHECK(req.body.find("\"visibility\":\"public\"") != std::string::npos);
}

TEST_CASE("LobbyRegistration: the posted body is escaped by the engine's one escaper (#1262)", "[lobby_reg]") {
    // This file used to carry its own escaper, which substituted a SPACE for every C0 control and
    // had no \b or \f. It produced valid JSON, so nothing broke -- but it was a second copy of the
    // one primitive #1080 centralised, and it had already drifted. Nothing pinned the bytes, which
    // is how the copy survived the #1161 sweep of the same directory.
    NullLog log;
    TrackingHttpClient http;
    http.setResponse("https://lobby.example/v1/servers", "", 200);
    ManualClock clock;
    LobbyRegistration reg(http, log);
    reg.setClock(clock);

    LobbyRegistrationConfig c = makeCfg();
    c.name = "quote\" back\\ slash";
    c.mode = "tab\there";
    c.mission = "bell\x07 and \x1f";
    reg.configure(c);

    reg.tick();
    REQUIRE(http.requests.size() == 1u);
    const std::string& body = http.requests.back().body;

    CHECK(body.find("\"name\":\"quote\\\" back\\\\ slash\"") != std::string::npos);
    CHECK(body.find("\"mode\":\"tab\\there\"") != std::string::npos);
    // The drift, pinned: C0 controls become \uXXXX rather than a space that loses the byte.
    CHECK(body.find("\"mission\":\"bell\\u0007 and \\u001f\"") != std::string::npos);
    CHECK(body.find(' ') != std::string::npos); // the legitimate space in "quote\" back" survives
}

TEST_CASE("LobbyRegistration: heartbeats on the configured interval, not every tick (#143)", "[lobby_reg]") {
    NullLog log;
    TrackingHttpClient http;
    http.setResponse("https://lobby.example/v1/servers", "", 200);
    ManualClock clock;
    LobbyRegistration reg(http, log);
    reg.setClock(clock);
    reg.configure(makeCfg());

    reg.tick(); // POST #1
    http.service();
    reg.tick(); // too soon
    CHECK(http.requests.size() == 1u);
    clock.advance(std::chrono::seconds(31));
    reg.tick(); // POST #2
    CHECK(http.requests.size() == 2u);
}

TEST_CASE("LobbyRegistration: disabled when private or lobby url empty (#143)", "[lobby_reg]") {
    NullLog log;
    TrackingHttpClient http;
    ManualClock clock;
    LobbyRegistration reg(http, log);
    reg.setClock(clock);

    LobbyRegistrationConfig priv = makeCfg();
    priv.visibilityPublic = false;
    reg.configure(priv);
    CHECK_FALSE(reg.enabled());
    reg.tick();
    CHECK(http.requests.empty());

    LobbyRegistrationConfig noUrl = makeCfg();
    noUrl.lobbyUrl.clear();
    reg.configure(noUrl);
    CHECK_FALSE(reg.enabled());
    reg.tick();
    CHECK(http.requests.empty());
}

TEST_CASE("LobbyRegistration: a failed POST backs off before retrying (#143)", "[lobby_reg]") {
    NullLog log;
    TrackingHttpClient http;
    http.setResponse("https://lobby.example/v1/servers", "", 500); // server error
    ManualClock clock;
    LobbyRegistration reg(http, log);
    reg.setClock(clock);
    reg.configure(makeCfg());

    reg.tick();     // POST #1 (in flight)
    http.service(); // completes 500 -> backoff x2 (60 s)
    CHECK(http.requests.size() == 1u);
    clock.advance(std::chrono::seconds(31)); // < 60 s backoff
    reg.tick();
    CHECK(http.requests.size() == 1u);       // still backing off
    clock.advance(std::chrono::seconds(31)); // now past 60 s
    reg.tick();
    CHECK(http.requests.size() == 2u);
}

TEST_CASE("LobbyRegistration: deregister sends a DELETE (#143)", "[lobby_reg]") {
    NullLog log;
    TrackingHttpClient http;
    ManualClock clock;
    LobbyRegistration reg(http, log);
    reg.setClock(clock);
    reg.configure(makeCfg());
    reg.deregister();
    REQUIRE(http.requests.size() == 1u);
    CHECK(http.requests.back().method == HttpMethod::Delete_);
    CHECK(http.requests.back().url == "https://lobby.example/v1/servers");
}
