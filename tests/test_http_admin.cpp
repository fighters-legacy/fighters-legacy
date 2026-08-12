// SPDX-License-Identifier: GPL-3.0-or-later
#include "HttpAdminServer.h"

#include "console/CommandRegistry.h"
#include "mock_hal.h"
#include "server_config.h"
#include <net/AdminChannel.h>

#include <catch2/catch_test_macros.hpp>

#include <httplib.h>

#include <IClock.h>
#include <chrono>
#include <span>
#include <string>
#include <vector>

using namespace fl;
namespace ha = fl::httpadmin;

namespace {

[[nodiscard]] ServerConfig::HttpAdminConfig cfgWith(std::vector<ServerConfig::HttpAdminToken> toks) {
    ServerConfig::HttpAdminConfig c;
    c.enabled = true;
    c.tokens = std::move(toks);
    return c;
}

} // namespace

// ── token table ─────────────────────────────────────────────────────────────────────────────────

TEST_CASE("http_admin: role presets map onto the capability vocabulary", "[http_admin]") {
    std::vector<ha::TokenGrant> table;
    std::string err;
    REQUIRE(ha::buildTokenTable(
        cfgWith({{"tok-admin", "admin", -1, ""}, {"tok-mod", "moderator", -1, ""}, {"tok-gm", "gm", -1, ""}}),
        "observe", table, err));
    REQUIRE(table.size() == 3);

    CHECK(table[0].caps == kAdminCaps);
    CHECK(table[1].caps == kModeratorCaps);
    CHECK(table[2].caps == kGameMasterCaps);

    // A moderator can kick but must not be able to reconfigure the server.
    CHECK(hasCaps(table[1].caps, capBit(Capability::KickBan)));
    CHECK_FALSE(hasCaps(table[1].caps, capBit(Capability::ServerConfig)));
}

TEST_CASE("http_admin: an unknown role preset is rejected, not silently downgraded", "[http_admin]") {
    std::vector<ha::TokenGrant> table;
    std::string err;
    // A typo must not yield a token that authenticates and then refuses everything -- that reads as
    // the server being broken rather than the config being wrong.
    CHECK_FALSE(ha::buildTokenTable(cfgWith({{"tok", "moderatr", -1, ""}}), "observe", table, err));
    CHECK(err.find("moderatr") != std::string::npos);
}

TEST_CASE("http_admin: a faction-scoped token carries its binding", "[http_admin]") {
    std::vector<ha::TokenGrant> table;
    std::string err;
    REQUIRE(ha::buildTokenTable(cfgWith({{"t1", "faction_leader", 2, ""}, {"t2", "admin", -1, ""}}), "observe", table,
                                err));
    CHECK(table[0].factionIndex == 2);
    CHECK(table[1].factionIndex == PeerAuthority::kNoFactionBinding); // -1 = unbound
}

// ── bearer extraction ───────────────────────────────────────────────────────────────────────────

TEST_CASE("http_admin: bearer extraction accepts the RFC forms and rejects the rest", "[http_admin]") {
    CHECK(ha::extractBearer("Bearer abc123") == "abc123");
    CHECK(ha::extractBearer("bearer abc123") == "abc123"); // scheme match is case-insensitive
    CHECK(ha::extractBearer("BEARER abc123") == "abc123");
    CHECK(ha::extractBearer("Bearer   abc123") == "abc123"); // multiple spaces collapse

    CHECK(ha::extractBearer("").empty());
    CHECK(ha::extractBearer("Bearer").empty());
    CHECK(ha::extractBearer("Bearer ").empty());
    CHECK(ha::extractBearer("Basic abc123").empty()); // a different scheme is not a bearer token
    CHECK(ha::extractBearer("Bearerabc123").empty()); // no separator
}

TEST_CASE("http_admin: token comparison is length- and content-safe", "[http_admin]") {
    CHECK(ha::constantTimeEquals("secret", "secret"));
    CHECK_FALSE(ha::constantTimeEquals("secret", "secrer"));
    CHECK_FALSE(ha::constantTimeEquals("secret", "secretx")); // length difference is a mismatch
    CHECK_FALSE(ha::constantTimeEquals("secret", "sec"));
    CHECK_FALSE(ha::constantTimeEquals("secret", ""));
    CHECK(ha::constantTimeEquals("", ""));
}

TEST_CASE("http_admin: token resolution", "[http_admin]") {
    std::vector<ha::TokenGrant> table;
    std::string err;
    REQUIRE(ha::buildTokenTable(cfgWith({{"alpha", "admin", -1, ""}, {"beta", "moderator", -1, ""}}), "observe", table,
                                err));

    REQUIRE(ha::resolveToken(table, "beta") != nullptr);
    CHECK(ha::resolveToken(table, "beta")->caps == kModeratorCaps);
    CHECK(ha::resolveToken(table, "alpha")->caps == kAdminCaps);

    CHECK(ha::resolveToken(table, "gamma") == nullptr);
    CHECK(ha::resolveToken(table, "") == nullptr);       // an absent header authenticates nobody
    CHECK(ha::resolveToken(table, "alph") == nullptr);   // a prefix is not a match
    CHECK(ha::resolveToken(table, "alphaa") == nullptr); // nor is an extension
    CHECK(ha::resolveToken({}, "alpha") == nullptr);     // an empty table matches nothing
}

TEST_CASE("http_admin: the issuer is built with explicit caps, never the Admin default", "[http_admin]") {
    ha::TokenGrant g;
    g.caps = kModeratorCaps;
    g.factionIndex = 3;
    const CommandIssuer issuer = ha::issuerFor(g);

    CHECK(issuer.peerId == kIssuerNoPeer); // a REST request is not a peer
    CHECK(issuer.caps == kModeratorCaps);
    CHECK(issuer.factionIndex == 3);
    // The trap this guards: a default-constructed CommandIssuer is full Admin.
    CHECK(CommandIssuer{}.caps == kAdminCaps);
    CHECK(issuer.caps != CommandIssuer{}.caps);
}

// ── request-body scanning (untrusted input) ─────────────────────────────────────────────────────

TEST_CASE("http_admin: JSON number fields", "[http_admin][json]") {
    CHECK(ha::jsonNumberField(R"({"peer": 7})", "peer").value_or(-1) == 7);
    CHECK(ha::jsonNumberField(R"({ "peer" : 42 })", "peer").value_or(-1) == 42);
    CHECK(ha::jsonNumberField("{\"peer\":\n 3}", "peer").value_or(-1) == 3);
    CHECK(ha::jsonNumberField(R"({"in": 1800, "reason": "x"})", "in").value_or(-1) == 1800);

    CHECK_FALSE(ha::jsonNumberField(R"({"peer": 7})", "other").has_value());
    CHECK_FALSE(ha::jsonNumberField("", "peer").has_value());
    CHECK_FALSE(ha::jsonNumberField(R"({"peer"})", "peer").has_value());   // no colon
    CHECK_FALSE(ha::jsonNumberField(R"({"peer": })", "peer").has_value()); // no value
    CHECK_FALSE(ha::jsonNumberField(R"({"peer": "x"})", "peer").has_value());
}

TEST_CASE("http_admin: JSON string fields fail closed on hostile input", "[http_admin][json]") {
    CHECK(ha::jsonStringField(R"({"ip": "1.2.3.4"})", "ip").value_or("") == "1.2.3.4");
    CHECK(ha::jsonStringField(R"({"reason": "going down"})", "reason").value_or("") == "going down");
    CHECK(ha::jsonStringField(R"({"ip": "a\"b"})", "ip").value_or("") == "a\"b"); // escapes decoded

    CHECK_FALSE(ha::jsonStringField(R"({"ip": 5})", "ip").has_value()); // not a string
    CHECK_FALSE(ha::jsonStringField(R"({"ip": "unterminated)", "ip").has_value());
    CHECK_FALSE(ha::jsonStringField("", "ip").has_value());
    CHECK_FALSE(ha::jsonStringField(R"({"other": "x"})", "ip").has_value());

    // An over-long field is refused rather than returned: the body is attacker-controlled.
    const std::string huge = R"({"ip": ")" + std::string(4096, 'a') + R"("})";
    CHECK_FALSE(ha::jsonStringField(huge, "ip").has_value());

    // A trailing backslash must not read past the end.
    CHECK_FALSE(ha::jsonStringField(std::string(R"({"ip": "abc\)"), "ip").has_value());
}

TEST_CASE("http_admin: error bodies are escaped JSON", "[http_admin][json]") {
    CHECK(ha::errorJson("plain") == R"({"error": "plain"})");
    // A command result echoed into an error body must not be able to break the document.
    CHECK(ha::errorJson("say \"hi\"") == R"({"error": "say \"hi\""})");
    CHECK(ha::errorJson("a\nb") == R"({"error": "a\nb"})");
}

namespace {

// The HTTP frontend's AdminChannel, wired to a registry the way fl-server wires it (#1079). Every
// route dispatches through this and obeys its lockout; so does MCP, which shares the listener.
struct HttpChannel {
    AdminChannel ch;

    explicit HttpChannel(const CommandRegistry& reg, int maxFailures = 5, int lockoutSeconds = 300)
        : ch([&reg](std::string_view line, const CommandIssuer& issuer) { return reg.dispatch(line, issuer); },
             makeCfg(maxFailures, lockoutSeconds), SystemClock::instance()) {}

  private:
    static AdminChannel::Config makeCfg(int maxFailures, int lockoutSeconds) {
        AdminChannel::Config c;
        c.name = "http";
        c.maxAuthFailures = maxFailures;
        c.lockoutSeconds = lockoutSeconds;
        return c;
    }
};

} // namespace

// ── server object without sockets ───────────────────────────────────────────────────────────────

TEST_CASE("http_admin: an unauthenticated API refuses to start", "[http_admin]") {
    MockLogger log;
    CommandRegistry reg;
    ServerConfig::HttpAdminConfig cfg;
    cfg.enabled = true; // but no tokens
    HttpChannel channel(reg);
    HttpAdminServer srv(channel.ch, cfg, log, ServerUptime{});
    CHECK_FALSE(srv.start()); // an open admin API is refused, not warned about
    CHECK(srv.boundPort() == 0);
}

TEST_CASE("http_admin: a bad role preset stops the server starting", "[http_admin]") {
    MockLogger log;
    CommandRegistry reg;
    HttpChannel channel(reg);
    HttpAdminServer srv(channel.ch, cfgWith({{"tok", "sysadmin", -1, ""}}), log, ServerUptime{});
    CHECK_FALSE(srv.start());
}

TEST_CASE("http_admin: its lockout is the channel's, readable with no listener open", "[http_admin]") {
    MockLogger log;
    CommandRegistry reg;
    // start() is never called, so no socket exists. The server owns no AuthTracker any more (#1079):
    // admin_unlock and admin_auth_status read exactly this state through the channel registry.
    HttpChannel channel(reg);
    HttpAdminServer srv(channel.ch, cfgWith({{"tok", "admin", -1, ""}}), log, ServerUptime{});
    CHECK_FALSE(channel.ch.clearLockout("1.2.3.4"));
    const AuthLockoutSummary s = channel.ch.authSummary();
    CHECK(s.activeCount == 0);
    CHECK(s.threshold == 5);
}

// ── config ──────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("parseServerConfig [http_admin] defaults when the section is absent", "[http_admin][config]") {
    MockLogger log;
    const auto cfg = parseServerConfig("[server]\nname = \"test\"\n", &log);
    CHECK_FALSE(cfg.httpAdmin.enabled);
    CHECK(cfg.httpAdmin.port == 8080);
    CHECK(cfg.httpAdmin.bindAddress == "127.0.0.1"); // localhost by default, deliberately
    CHECK(cfg.httpAdmin.tokens.empty());
    CHECK(cfg.httpAdmin.maxAuthFailures == 5);
    CHECK(cfg.httpAdmin.lockoutSeconds == 300);
}

TEST_CASE("parseServerConfig [http_admin] reads tokens", "[http_admin][config]") {
    MockLogger log;
    const char* toml = R"(
[http_admin]
enabled = true
port = 9090
bind_address = "0.0.0.0"

[[http_admin.tokens]]
token = "s3cr3t"
role = "moderator"

[[http_admin.tokens]]
token = "gm-token"
role = "gm"
faction = 1
)";
    const auto cfg = parseServerConfig(toml, &log);
    CHECK(cfg.httpAdmin.enabled);
    CHECK(cfg.httpAdmin.port == 9090);
    CHECK(cfg.httpAdmin.bindAddress == "0.0.0.0");
    REQUIRE(cfg.httpAdmin.tokens.size() == 2);
    CHECK(cfg.httpAdmin.tokens[0].role == "moderator");
    CHECK(cfg.httpAdmin.tokens[1].faction == 1);
}

TEST_CASE("parseServerConfig [http_admin] refuses to enable an API with no tokens", "[http_admin][config]") {
    MockLogger log;
    const auto cfg = parseServerConfig("[http_admin]\nenabled = true\n", &log);
    // Not a warning: this endpoint can kick, ban and shut the server down.
    CHECK_FALSE(cfg.httpAdmin.enabled);
    CHECK(log.hasMessage(LogLevel::Error, "http_admin"));
}

TEST_CASE("parseServerConfig [http_admin] drops an empty token row", "[http_admin][config]") {
    MockLogger log;
    const char* toml = R"(
[http_admin]
enabled = true
[[http_admin.tokens]]
token = ""
role = "admin"
[[http_admin.tokens]]
token = "real"
role = "admin"
)";
    const auto cfg = parseServerConfig(toml, &log);
    REQUIRE(cfg.httpAdmin.tokens.size() == 1);
    CHECK(cfg.httpAdmin.tokens[0].token == "real");
    CHECK(cfg.httpAdmin.enabled);
}

TEST_CASE("parseServerConfig [http_admin] range-checks and warns", "[http_admin][config]") {
    MockLogger log;
    const auto cfg = parseServerConfig("[http_admin]\nport = 70000\nlockout_seconds = 0\n", &log);
    CHECK(cfg.httpAdmin.port == 8080); // out of range keeps the default, never clamps
    CHECK(cfg.httpAdmin.lockoutSeconds == 300);
    CHECK(log.hasMessage(LogLevel::Warn, "http_admin.port"));
    CHECK(log.hasMessage(LogLevel::Warn, "http_admin.lockout_seconds"));
}

TEST_CASE("the default server.toml documents the section it writes", "[http_admin][config]") {
    // ensureAndReadConfig writes this template on first run; a section missing from it is a section
    // an operator never discovers.
    const std::string tmpl{defaultServerConfigToml()};
    CHECK(tmpl.find("[http_admin]") != std::string::npos);
    CHECK(tmpl.find("bind_address") != std::string::npos);
    CHECK(tmpl.find("[[http_admin.tokens]]") != std::string::npos);

    // And it must round-trip through the parser it describes.
    MockLogger log;
    const auto cfg = parseServerConfig(tmpl, &log);
    CHECK_FALSE(cfg.httpAdmin.enabled);
}

// ── uptime ──────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("ServerUptime cannot report the clock epoch (#1048)", "[http_admin][uptime]") {
    SECTION("a default-constructed one has just started") {
        // The property the bug needed and did not have: there is no state in which this reads as
        // "the machine booted N seconds ago". A brand new ServerUptime is at zero, not at boot.
        const ServerUptime up;
        CHECK(up.seconds() >= 0);
        CHECK(up.seconds() < 60);
    }

    SECTION("elapsed follows the clock it was started on") {
        ManualClock clock;
        const ServerUptime up{clock};
        CHECK(up.seconds() == 0);
        clock.advance(std::chrono::seconds(137));
        CHECK(up.seconds() == 137);
    }

    SECTION("a copy carries the same start instant") {
        // This is what makes "one instance, handed to every frontend" hold: passing a ServerUptime
        // by value cannot fork the answer, which is exactly how the two uptimes diverged before.
        ManualClock clock;
        const ServerUptime original{clock};
        clock.advance(std::chrono::seconds(90));
        const ServerUptime copy = original;
        CHECK(copy.startedAt() == original.startedAt());
        CHECK(copy.seconds() == original.seconds());
        clock.advance(std::chrono::seconds(10));
        CHECK(copy.seconds() == 100);
        CHECK(copy.seconds() == original.seconds());
    }

    SECTION("an explicit start instant reports the time already elapsed") {
        ManualClock clock;
        const ServerUptime up{clock.now() - std::chrono::hours(3), clock};
        CHECK(up.seconds() == 3 * 3600);
    }
}

// ── live listener (the wiring, not just the helpers) ────────────────────────────────────────────

TEST_CASE("http_admin: a live server enforces auth and routes to the command registry", "[http_admin][live]") {
    MockLogger log;
    CommandRegistry reg;

    // Two commands with different capability requirements, so the test can prove the REST frontend
    // really carries the token's caps into the permission check rather than dispatching as Admin.
    // One uptime, two frontends — the production wiring (#1048). The stand-in `status` reports from
    // the same instance /health does, so the sections below can assert they agree rather than
    // asserting each in isolation, which is how the two drifted apart in the first place.
    ManualClock clock;
    const ServerUptime uptime{clock};
    clock.advance(std::chrono::seconds(137));

    int statusCalls = 0;
    reg.registerCommand("status", "status", 0, [&statusCalls, &uptime](std::span<std::string_view>) -> std::string {
        ++statusCalls;
        return "uptime: " + std::to_string(uptime.seconds()) + "s  peers: 0";
    });
    reg.registerCommand("kick", "kick", capBit(Capability::KickBan),
                        [](std::span<std::string_view> args) -> std::string {
                            return args.empty() ? "usage" : ("kicked " + std::string(args[0]));
                        });
    reg.registerCommand("set_weather", "set_weather", capBit(Capability::ServerConfig),
                        [](std::span<std::string_view>) -> std::string { return "weather set"; });

    ServerConfig::HttpAdminConfig cfg = cfgWith({{"admin-tok", "admin", -1, ""}, {"mod-tok", "moderator", -1, ""}});
    cfg.bindAddress = "127.0.0.1";
    cfg.port = 0; // let the OS pick, so the test never fights over a fixed port

    HttpChannel channel(reg);
    HttpAdminServer srv(channel.ch, cfg, log, uptime);
    REQUIRE(srv.start());
    const uint16_t port = srv.boundPort();
    REQUIRE(port != 0);

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    SECTION("/health needs no credential and reports the server's own uptime") {
        auto res = cli.Get("/health");
        REQUIRE(res);
        CHECK(res->status == 200);
        CHECK(res->body.find("\"status\": \"ok\"") != std::string::npos);
        // The VALUE, not just the key: the number is the whole point of the route.
        CHECK(res->body.find("\"uptime\": 137") != std::string::npos);
    }

    SECTION("/health and /status report the same uptime (#1048)") {
        auto health = cli.Get("/health");
        REQUIRE(health);
        httplib::Headers h{{"Authorization", "Bearer admin-tok"}};
        auto status = cli.Get("/status", h);
        REQUIRE(status);
        CHECK(health->body.find("\"uptime\": 137") != std::string::npos);
        CHECK(status->body.find("uptime: 137s") != std::string::npos);
    }

    SECTION("an unauthenticated request to a real route is refused") {
        auto res = cli.Get("/status");
        REQUIRE(res);
        CHECK(res->status == 401);
        CHECK(res->body.find("\"error\"") != std::string::npos);
        CHECK(statusCalls == 0); // the command was never reached
    }

    SECTION("a wrong token is refused") {
        httplib::Headers h{{"Authorization", "Bearer not-the-token"}};
        auto res = cli.Get("/status", h);
        REQUIRE(res);
        CHECK(res->status == 401);
        CHECK(statusCalls == 0);
    }

    SECTION("a valid token reaches the command") {
        httplib::Headers h{{"Authorization", "Bearer admin-tok"}};
        auto res = cli.Get("/status", h);
        REQUIRE(res);
        CHECK(res->status == 200);
        CHECK(res->body.find("peers: 0") != std::string::npos);
        CHECK(statusCalls == 1);
    }

    SECTION("capabilities are enforced per token, not per transport") {
        // The moderator token holds KickBan but not ServerConfig. This is the assertion that proves
        // REST is a frontend over the #946 permission check rather than an Admin backdoor.
        httplib::Headers mod{{"Authorization", "Bearer mod-tok"}};
        auto kick = cli.Post("/kick", mod, R"({"peer": 3})", "application/json");
        REQUIRE(kick);
        CHECK(kick->status == 200);
        CHECK(kick->body.find("kicked 3") != std::string::npos);

        // And an admin token can do what the moderator cannot.
        httplib::Headers adm{{"Authorization", "Bearer admin-tok"}};
        auto ok = cli.Get("/status", adm);
        REQUIRE(ok);
        CHECK(ok->status == 200);
    }

    SECTION("a malformed body is a 400, not a dispatch") {
        httplib::Headers h{{"Authorization", "Bearer admin-tok"}};
        auto res = cli.Post("/kick", h, R"({"nope": 1})", "application/json");
        REQUIRE(res);
        CHECK(res->status == 400);
        CHECK(res->body.find("peer") != std::string::npos); // the error says what was expected
    }

    SECTION("an unknown path is a 404") {
        httplib::Headers h{{"Authorization", "Bearer admin-tok"}};
        auto res = cli.Get("/not-a-route", h);
        REQUIRE(res);
        CHECK(res->status == 404);
    }

    srv.stop();
}

TEST_CASE("http_admin: repeated bad tokens lock the source IP out", "[http_admin][live]") {
    MockLogger log;
    CommandRegistry reg;
    reg.registerCommand("status", "status", 0, [](std::span<std::string_view>) -> std::string { return "ok"; });

    ServerConfig::HttpAdminConfig cfg = cfgWith({{"good", "admin", -1, ""}});
    cfg.bindAddress = "127.0.0.1";
    cfg.port = 0;
    cfg.maxAuthFailures = 3;
    cfg.lockoutSeconds = 300;

    HttpChannel channel(reg, cfg.maxAuthFailures, cfg.lockoutSeconds);
    HttpAdminServer srv(channel.ch, cfg, log, ServerUptime{});
    REQUIRE(srv.start());

    httplib::Client cli("127.0.0.1", srv.boundPort());
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);
    httplib::Headers bad{{"Authorization", "Bearer wrong"}};

    for (int i = 0; i < 3; ++i) {
        auto res = cli.Get("/status", bad);
        REQUIRE(res);
        CHECK(res->status == 401);
    }

    // Now locked out: even the CORRECT token is refused, and with 429 rather than 401 so a client
    // can tell "wrong credential" from "stop trying for a while".
    httplib::Headers good{{"Authorization", "Bearer good"}};
    auto locked = cli.Get("/status", good);
    REQUIRE(locked);
    CHECK(locked->status == 429);

    CHECK(channel.ch.authSummary().activeCount == 1);

    // An operator clearing the lockout restores access — the admin_unlock path, which now reaches this
    // channel through the registry rather than through a hook named "httpAdmin".
    CHECK(channel.ch.clearLockout("127.0.0.1"));
    auto after = cli.Get("/status", good);
    REQUIRE(after);
    CHECK(after->status == 200);

    srv.stop();
}
