// SPDX-License-Identifier: GPL-3.0-or-later
#include "HttpAdminServer.h"
#include "McpEndpoint.h"
#include "McpProtocol.h"
#include "console/CommandRegistry.h"
#include "mock_hal.h"
#include "server_config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fl;
namespace ha = fl::httpadmin;

// MCP surface tests (#601).
//
// Everything here runs WITHOUT a socket: McpEndpoint is transport-free by design, so a tier refusal,
// an allowlist refusal and a capability refusal can each be provoked directly. That separation is
// the thing under test — MCP is a frontend over CommandRegistry::dispatch, not a parallel admin
// path, and the way to prove it is to show a command refused by the capability mask even when the
// tier and the allowlist both permitted the attempt.

namespace {

// A registry with one capability-gated command, so the "the tier let it through and the capability
// mask still refused it" case is reachable.
class Fixture {
  public:
    Fixture() {
        m_registry.registerCommand("status", "server status", CapabilityMask{0},
                                   [](std::span<std::string_view>) { return std::string("peers: 0"); });
        // shutdown demands ServerConfig, exactly as the real one does.
        m_registry.registerCommand("shutdown", "stop the server", capBit(Capability::ServerConfig),
                                   [](std::span<std::string_view>) { return std::string("stopping"); });
        m_registry.registerCommand("worldstate", "world state", CapabilityMask{0}, [](std::span<std::string_view>) {
            return std::string(R"({"tick": 42, "entities": []})");
        });
        m_registry.registerCommand("events", "match events", CapabilityMask{0}, [](std::span<std::string_view>) {
            return std::string(R"({"events": [], "nextSeq": 1, "gap": false})");
        });
    }

    [[nodiscard]] CommandRegistry& registry() {
        return m_registry;
    }

  private:
    CommandRegistry m_registry;
};

// One logger for the whole file; nothing here asserts on its contents.
MockLogger& testLogger() {
    static MockLogger log;
    return log;
}

[[nodiscard]] ha::TokenGrant grantAt(mcp::Autonomy tier, CapabilityMask caps = kAdminCaps) {
    ha::TokenGrant g;
    g.token = "tok";
    g.caps = caps;
    g.autonomy = tier;
    return g;
}

[[nodiscard]] ServerConfig::McpConfig cfgWith(std::vector<std::string> allowlist) {
    ServerConfig::McpConfig c;
    c.enabled = true;
    c.allowlist = std::move(allowlist);
    return c;
}

// Drive initialize and return the session id, so every later call has a session to present.
[[nodiscard]] std::string openSession(McpEndpoint& ep, const ha::TokenGrant& g) {
    const auto out = ep.handle(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize"})", g, {});
    return out.newSessionId;
}

} // namespace

// ── the scanner ─────────────────────────────────────────────────────────────────────────────────

TEST_CASE("mcp: the member scanner reads one object level", "[mcp]") {
    constexpr std::string_view obj = R"({"a": 1, "b": "two", "c": {"d": 3}})";
    CHECK(mcp::intValue(mcp::objectMember(obj, "a")) == 1);
    CHECK(mcp::stringValue(mcp::objectMember(obj, "b")) == "two");
    CHECK(mcp::objectMember(obj, "c") == R"({"d": 3})");
    // Nested keys are NOT visible at this level: a flat substring search would have found "d".
    CHECK(mcp::objectMember(obj, "d").empty());
}

TEST_CASE("mcp: a key spelled inside a string value is not a member", "[mcp]") {
    // The attack the depth-aware scanner exists to stop: an attacker-controlled string value that
    // contains what looks like a member the server is about to look up.
    constexpr std::string_view obj = R"({"chat": "\"command\": \"shutdown\"", "command": "status"})";
    CHECK(mcp::stringValue(mcp::objectMember(obj, "command")) == "status");
}

TEST_CASE("mcp: the scanner fails closed on malformed input", "[mcp]") {
    CHECK(mcp::objectMember(R"({"a": )", "a").empty());     // truncated value
    CHECK(mcp::objectMember(R"({"a" 1})", "a").empty());    // missing colon
    CHECK(mcp::objectMember(R"({a: 1})", "a").empty());     // unquoted key
    CHECK(mcp::objectMember(R"([{"a": 1}])", "a").empty()); // an array, not an object
    CHECK(mcp::objectMember(R"({"a": "unterminated)", "a").empty());
    CHECK_FALSE(mcp::stringValue(R"("bad \q escape")").has_value());
}

TEST_CASE("mcp: an over-long string value is refused rather than truncated", "[mcp]") {
    std::string big = "\"";
    big.append(mcp::kMaxStringValue + 16, 'x');
    big += "\"";
    // Truncating would hand the caller a value that looks complete and is not.
    CHECK_FALSE(mcp::stringValue(big).has_value());
}

TEST_CASE("mcp: unicode escapes decode, and a lone surrogate does not produce invalid UTF-8", "[mcp]") {
    CHECK(mcp::stringValue(R"("A")") == "A");
    const auto lone = mcp::stringValue(R"("\ud800")");
    REQUIRE(lone.has_value());
    CHECK(*lone == "\xEF\xBF\xBD"); // U+FFFD
}

// ── the envelope ────────────────────────────────────────────────────────────────────────────────

TEST_CASE("mcp: a well-formed request parses", "[mcp]") {
    mcp::Request req;
    std::string err;
    REQUIRE(mcp::parseRequest(R"({"jsonrpc": "2.0", "id": 7, "method": "tools/list"})", req, err));
    CHECK(req.id == "7");
    CHECK(req.method == "tools/list");
    CHECK_FALSE(req.isNotification);
}

TEST_CASE("mcp: a string id is echoed back as a string", "[mcp]") {
    mcp::Request req;
    std::string err;
    REQUIRE(mcp::parseRequest(R"({"jsonrpc": "2.0", "id": "abc", "method": "ping"})", req, err));
    CHECK(req.id == "\"abc\"");
    // Echoed verbatim, so a client that used a string id does not get a number back.
    CHECK(mcp::resultResponse(req.id, "{}").find("\"id\": \"abc\"") != std::string::npos);
}

TEST_CASE("mcp: a request with no id is a notification", "[mcp]") {
    mcp::Request req;
    std::string err;
    REQUIRE(mcp::parseRequest(R"({"jsonrpc": "2.0", "method": "notifications/initialized"})", req, err));
    CHECK(req.isNotification);
}

TEST_CASE("mcp: batching is refused, and says so", "[mcp]") {
    mcp::Request req;
    std::string err;
    // Removed in revision 2025-06-18. A generic parse error would leave a client debugging its JSON.
    CHECK_FALSE(mcp::parseRequest(R"([{"jsonrpc": "2.0", "id": 1, "method": "ping"}])", req, err));
    CHECK(err.find("batching") != std::string::npos);
    CHECK(err.find("-32600") != std::string::npos);
}

TEST_CASE("mcp: malformed envelopes are rejected with a sendable error body", "[mcp]") {
    mcp::Request req;
    std::string err;
    CHECK_FALSE(mcp::parseRequest("", req, err));
    CHECK(err.find("-32700") != std::string::npos);
    CHECK_FALSE(mcp::parseRequest(R"({"id": 1, "method": "ping"})", req, err)); // no jsonrpc
    CHECK(err.find("-32600") != std::string::npos);
    CHECK_FALSE(mcp::parseRequest(R"({"jsonrpc": "1.0", "id": 1, "method": "ping"})", req, err));
    CHECK_FALSE(mcp::parseRequest(R"({"jsonrpc": "2.0", "id": 1})", req, err)); // no method
    CHECK_FALSE(mcp::parseRequest(R"({"jsonrpc": "2.0", "id": 1, "method": "ping", "params": [1]})", req, err));
    CHECK(err.find("-32602") != std::string::npos); // params must be an object
}

// ── allowlist ───────────────────────────────────────────────────────────────────────────────────

TEST_CASE("mcp: an empty allowlist permits nothing", "[mcp]") {
    // The load-bearing default. Reading "empty means unrestricted" would turn a config omission into
    // a remote shell.
    const std::vector<std::string> none;
    CHECK_FALSE(mcp::commandAllowed(none, "status"));
}

TEST_CASE("mcp: the allowlist matches the verb, not a prefix of the line", "[mcp]") {
    const std::vector<std::string> allow{"status", "peers"};
    CHECK(mcp::commandAllowed(allow, "status"));
    CHECK(mcp::commandAllowed(allow, "  status   extra args "));
    CHECK_FALSE(mcp::commandAllowed(allow, "status_secret"));
    CHECK_FALSE(mcp::commandAllowed(allow, "shutdown"));
    CHECK_FALSE(mcp::commandAllowed(allow, ""));
    CHECK_FALSE(mcp::commandAllowed(allow, "   "));
}

// ── autonomy tiers ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("mcp: autonomy tiers parse and order", "[mcp]") {
    CHECK(mcp::parseAutonomy("observe") == mcp::Autonomy::Observe);
    CHECK(mcp::parseAutonomy("act") == mcp::Autonomy::Act);
    CHECK_FALSE(mcp::parseAutonomy("Act").has_value()); // case-sensitive; a typo is not a downgrade
    CHECK_FALSE(mcp::parseAutonomy("").has_value());
    CHECK(static_cast<uint8_t>(mcp::Autonomy::Observe) < static_cast<uint8_t>(mcp::Autonomy::Act));
}

TEST_CASE("mcp: an unknown autonomy on a token row is rejected at startup", "[mcp]") {
    ServerConfig::HttpAdminConfig cfg;
    cfg.enabled = true;
    ServerConfig::HttpAdminToken row;
    row.token = "t";
    row.role = "admin";
    row.autonomy = "acct";
    cfg.tokens.push_back(row);

    std::vector<ha::TokenGrant> table;
    std::string err;
    CHECK_FALSE(ha::buildTokenTable(cfg, "observe", table, err));
    CHECK(err.find("acct") != std::string::npos);
}

TEST_CASE("mcp: an unknown default autonomy is rejected before any row inherits it", "[mcp]") {
    ServerConfig::HttpAdminConfig cfg;
    cfg.enabled = true;
    cfg.tokens.push_back({"t", "admin", -1, ""});
    std::vector<ha::TokenGrant> table;
    std::string err;
    CHECK_FALSE(ha::buildTokenTable(cfg, "observ", table, err));
    CHECK(err.find("observ") != std::string::npos);
}

TEST_CASE("mcp: a row with no autonomy inherits the default", "[mcp]") {
    ServerConfig::HttpAdminConfig cfg;
    cfg.enabled = true;
    cfg.tokens.push_back({"t1", "admin", -1, ""});
    cfg.tokens.push_back({"t2", "admin", -1, "act"});
    std::vector<ha::TokenGrant> table;
    std::string err;
    REQUIRE(ha::buildTokenTable(cfg, "recommend", table, err));
    CHECK(table[0].autonomy == mcp::Autonomy::Recommend);
    CHECK(table[1].autonomy == mcp::Autonomy::Act);
}

// ── dispatch ────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("mcp: initialize reports the pinned revision and opens a session", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    const auto out =
        ep.handle(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize"})", grantAt(mcp::Autonomy::Observe), {});
    CHECK(out.httpStatus == 200);
    CHECK_FALSE(out.newSessionId.empty());
    CHECK(out.body.find(mcp::kProtocolRevision) != std::string::npos);
    CHECK(out.body.find("\"subscribe\": true") != std::string::npos);
    CHECK(ep.sessionCount() == 1);
}

TEST_CASE("mcp: a call without a session is refused", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    // A bearer token alone must not be enough to start calling tools: the handshake is where the
    // revision is agreed and the session is bound.
    const auto out =
        ep.handle(R"({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})", grantAt(mcp::Autonomy::Observe), {});
    CHECK(out.httpStatus == 404);
    CHECK(out.body.find("initialize") != std::string::npos);
}

TEST_CASE("mcp: tools/list hides tools the caller's tier can never reach", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({"status"}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Observe);
    const std::string sid = openSession(ep, g);

    const auto out = ep.handle(R"({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})", g, sid);
    CHECK(out.body.find("world_state") != std::string::npos);
    CHECK(out.body.find("events") != std::string::npos);
    // An observe-tier agent should not spend a turn discovering it may not act.
    CHECK(out.body.find("admin_command") == std::string::npos);
    CHECK(out.body.find("submit_mission") == std::string::npos);
}

TEST_CASE("mcp: an observe token cannot call admin_command", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({"status"}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Observe);
    const std::string sid = openSession(ep, g);

    const auto out =
        ep.handle(R"({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "admin_command",)"
                  R"( "arguments": {"command": "status"}}})",
                  g, sid);
    CHECK(out.body.find("requires autonomy tier") != std::string::npos);
    CHECK(out.body.find("\"error\"") != std::string::npos);
}

TEST_CASE("mcp: an act token still cannot run a command off the allowlist", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({"status"}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Act);
    const std::string sid = openSession(ep, g);

    const auto out =
        ep.handle(R"({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "admin_command",)"
                  R"( "arguments": {"command": "shutdown"}}})",
                  g, sid);
    CHECK(out.body.find("not on this server's MCP allowlist") != std::string::npos);
    CHECK(out.body.find("\"isError\": true") != std::string::npos);
}

TEST_CASE("mcp: the capability mask refuses even when tier and allowlist both permit", "[mcp]") {
    // THE test for "MCP is a frontend, not a parallel admin path". Everything MCP owns says yes;
    // the #945 capability check inside CommandRegistry::dispatch is what says no.
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({"shutdown"}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Act, kModeratorCaps);
    const std::string sid = openSession(ep, g);

    const auto out =
        ep.handle(R"({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "admin_command",)"
                  R"( "arguments": {"command": "shutdown"}}})",
                  g, sid);
    CHECK(out.body.find("permission denied") != std::string::npos);
    CHECK(out.body.find("\"isError\": true") != std::string::npos);
}

TEST_CASE("mcp: an allowlisted command a capable token runs succeeds", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({"shutdown"}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Act, kAdminCaps);
    const std::string sid = openSession(ep, g);

    const auto out =
        ep.handle(R"({"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "admin_command",)"
                  R"( "arguments": {"command": "shutdown"}}})",
                  g, sid);
    CHECK(out.body.find("stopping") != std::string::npos);
    CHECK(out.body.find("\"isError\": false") != std::string::npos);
}

TEST_CASE("mcp: world_state comes back as structured content", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Observe);
    const std::string sid = openSession(ep, g);

    const auto out =
        ep.handle(R"({"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "world_state"}})", g, sid);
    CHECK(out.body.find("structuredContent") != std::string::npos);
    CHECK(out.body.find("\"tick\": 42") != std::string::npos);
    CHECK(out.body.find("\"isError\": false") != std::string::npos);
}

TEST_CASE("mcp: an unknown tool and an unknown method are distinct errors", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Act);
    const std::string sid = openSession(ep, g);

    const auto tool =
        ep.handle(R"({"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": "rm_rf"}})", g, sid);
    CHECK(tool.body.find("-32602") != std::string::npos); // bad params: no such tool

    const auto method = ep.handle(R"({"jsonrpc": "2.0", "id": 9, "method": "sudo/make_me_admin"})", g, sid);
    CHECK(method.body.find("-32601") != std::string::npos); // no such method
}

TEST_CASE("mcp: submit_mission validates without loading", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Recommend);
    const std::string sid = openSession(ep, g);

    const auto bad =
        ep.handle(R"({"jsonrpc": "2.0", "id": 10, "method": "tools/call", "params": {"name": "submit_mission",)"
                  R"( "arguments": {"yaml": "{{{ not yaml"}}})",
                  g, sid);
    // A schema failure is the ANSWER the agent asked for, so it is a tool result it can act on --
    // not a protocol error that reads as "the exchange is broken".
    CHECK(bad.body.find("\"isError\": true") != std::string::npos);
    CHECK(bad.body.find("\"ok\": false") != std::string::npos);
    CHECK(bad.body.find("-32603") == std::string::npos);

    const auto missing =
        ep.handle(R"({"jsonrpc": "2.0", "id": 11, "method": "tools/call", "params": {"name": "submit_mission",)"
                  R"( "arguments": {}}})",
                  g, sid);
    CHECK(missing.body.find("-32602") != std::string::npos);
}

TEST_CASE("mcp: an oversized mission document is refused before parsing", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Recommend);
    const std::string sid = openSession(ep, g);

    std::string body = R"({"jsonrpc": "2.0", "id": 12, "method": "tools/call", "params": {"name": "submit_mission",)"
                       R"( "arguments": {"yaml": ")";
    body.append(600 * 1024, 'x');
    body += "\"}}}";
    const auto out = ep.handle(body, g, sid);
    CHECK(out.body.find("too large") != std::string::npos);
}

// ── subscriptions and notifications ─────────────────────────────────────────────────────────────

TEST_CASE("mcp: a subscriber is notified when the world snapshot advances", "[mcp]") {
    Fixture f;
    uint64_t tick = 0;
    McpHooks hooks;
    hooks.worldStateTick = [&tick] { return tick; };
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), std::move(hooks));
    const auto g = grantAt(mcp::Autonomy::Observe);
    const std::string sid = openSession(ep, g);

    REQUIRE(ep.handle(R"({"jsonrpc": "2.0", "id": 13, "method": "resources/subscribe",)"
                      R"( "params": {"uri": "fl://world_state"}})",
                      g, sid)
                .httpStatus == 200);

    // No snapshot published yet: nothing to say.
    CHECK(ep.pollNotifications(sid).empty());

    tick = 120;
    const auto frames = ep.pollNotifications(sid);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].find("notifications/resources/updated") != std::string::npos);
    CHECK(frames[0].find("fl://world_state") != std::string::npos);

    // Same tick again: a subscriber must not be woken for a snapshot it already knows about.
    CHECK(ep.pollNotifications(sid).empty());

    tick = 180;
    CHECK(ep.pollNotifications(sid).size() == 1);
}

TEST_CASE("mcp: unsubscribing stops the notifications", "[mcp]") {
    Fixture f;
    uint64_t tick = 10;
    McpHooks hooks;
    hooks.worldStateTick = [&tick] { return tick; };
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), std::move(hooks));
    const auto g = grantAt(mcp::Autonomy::Observe);
    const std::string sid = openSession(ep, g);

    CHECK(
        ep.handle(
              R"({"jsonrpc": "2.0", "id": 1, "method": "resources/subscribe", "params": {"uri": "fl://world_state"}})",
              g, sid)
            .httpStatus == 200);
    tick = 20;
    CHECK(ep.pollNotifications(sid).size() == 1);

    CHECK(
        ep.handle(
              R"({"jsonrpc": "2.0", "id": 2, "method": "resources/unsubscribe", "params": {"uri": "fl://world_state"}})",
              g, sid)
            .httpStatus == 200);
    tick = 30;
    CHECK(ep.pollNotifications(sid).empty());
}

TEST_CASE("mcp: polling an unknown session yields nothing", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    CHECK_FALSE(ep.sessionExists("nope"));
    CHECK(ep.pollNotifications("nope").empty());
}

TEST_CASE("mcp: subscribing to an unknown resource is refused", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Observe);
    const std::string sid = openSession(ep, g);
    const auto out = ep.handle(
        R"({"jsonrpc": "2.0", "id": 1, "method": "resources/subscribe", "params": {"uri": "fl://etc/passwd"}})", g,
        sid);
    CHECK(out.body.find("-32602") != std::string::npos);
}

TEST_CASE("mcp: sessions are capped and the idlest is evicted", "[mcp]") {
    Fixture f;
    ServerConfig::McpConfig cfg = cfgWith({});
    cfg.maxSessions = 2;
    ManualClock clock;
    McpEndpoint ep(f.registry(), cfg, testLogger(), {});
    ep.setClock(clock);
    const auto g = grantAt(mcp::Autonomy::Observe);

    const std::string a = openSession(ep, g);
    clock.advance(std::chrono::seconds(1));
    const std::string b = openSession(ep, g);
    clock.advance(std::chrono::seconds(1));
    const std::string c = openSession(ep, g);

    CHECK(ep.sessionCount() == 2);
    CHECK_FALSE(ep.sessionExists(a)); // the idlest went
    CHECK(ep.sessionExists(b));
    CHECK(ep.sessionExists(c));
}

// ── rate limiting ───────────────────────────────────────────────────────────────────────────────

TEST_CASE("mcp: the rate limiter is per token and expires with the window", "[mcp]") {
    ManualClock clock;
    mcp::RateLimiter lim(2);
    lim.setClock(clock);

    CHECK(lim.allow("a"));
    CHECK(lim.allow("a"));
    CHECK_FALSE(lim.allow("a"));
    // A second token is unaffected: the limit is on the grant, not on the server.
    CHECK(lim.allow("b"));

    clock.advance(std::chrono::seconds(61));
    CHECK(lim.allow("a"));
}

TEST_CASE("mcp: a non-positive rate limit disables the limiter", "[mcp]") {
    mcp::RateLimiter lim(0);
    for (int i = 0; i < 1000; ++i)
        CHECK(lim.allow("a"));
}

TEST_CASE("mcp: pruning drops windows nobody is using", "[mcp]") {
    ManualClock clock;
    mcp::RateLimiter lim(5);
    lim.setClock(clock);
    CHECK(lim.allow("a"));
    CHECK(lim.trackedTokens() == 1);
    clock.advance(std::chrono::minutes(3));
    lim.pruneExpired();
    CHECK(lim.trackedTokens() == 0);
}

TEST_CASE("mcp: a rate-limited call is refused with 429", "[mcp]") {
    Fixture f;
    ServerConfig::McpConfig cfg = cfgWith({});
    cfg.rateLimitPerMin = 1;
    McpEndpoint ep(f.registry(), cfg, testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Observe);

    // initialize consumes the one call this token gets.
    const auto first = ep.handle(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize"})", g, {});
    CHECK(first.httpStatus == 200);
    const auto second = ep.handle(R"({"jsonrpc": "2.0", "id": 2, "method": "ping"})", g, first.newSessionId);
    CHECK(second.httpStatus == 429);
}

// ── catalog stability ───────────────────────────────────────────────────────────────────────────

TEST_CASE("mcp: every catalogued tool has a handler and a schema", "[mcp]") {
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({"status"}), testLogger(), {});
    const auto g = grantAt(mcp::Autonomy::Act);
    const std::string sid = openSession(ep, g);

    for (const mcp::ToolDesc& t : mcp::toolCatalog()) {
        CHECK_FALSE(t.inputSchema.empty());
        CHECK_FALSE(t.outputSchema.empty());
        CHECK(mcp::findTool(t.name) == &t);

        // "has no handler" is the dispatcher's own fallthrough: a tool added to the catalog and not
        // to the dispatcher must fail here rather than in production.
        std::string body = R"({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": ")";
        body += t.name;
        body += R"("}})";
        CHECK(ep.handle(body, g, sid).body.find("has no handler") == std::string::npos);
    }
}

TEST_CASE("mcp: the pinned revision is the one that supports what we advertise", "[mcp]") {
    // A bump is a deliberate act: batching support, structured output and subscriptions all key off
    // this string, and the endpoint reports it to every client.
    CHECK(mcp::kProtocolRevision == "2025-06-18");
}

TEST_CASE("mcp: a session is bound to the token that opened it", "[mcp]") {
    // Found while chasing an Apple-Clang -Wunused-private-field error: Session::token was stored,
    // never read, and carried a comment claiming a binding the code did not enforce. Authority is
    // re-derived from the presented grant on every request, so a borrowed session id could not
    // escalate anything — but one token driving another's subscription state is a confusion worth
    // refusing outright rather than reasoning about later.
    Fixture f;
    McpEndpoint ep(f.registry(), cfgWith({}), testLogger(), {});

    auto alice = grantAt(mcp::Autonomy::Observe);
    alice.token = "alice";
    auto bob = grantAt(mcp::Autonomy::Observe);
    bob.token = "bob";

    const std::string sid = openSession(ep, alice);
    REQUIRE_FALSE(sid.empty());

    // Alice's own session works.
    CHECK(ep.handle(R"({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})", alice, sid).httpStatus == 200);

    // Bob presenting Alice's session id is refused, even though Bob authenticated fine.
    const auto stolen = ep.handle(R"({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})", bob, sid);
    CHECK(stolen.httpStatus == 404);
    CHECK(stolen.body.find("no MCP session for this token") != std::string::npos);
}
