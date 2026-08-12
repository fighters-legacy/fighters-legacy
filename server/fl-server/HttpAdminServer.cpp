// SPDX-License-Identifier: GPL-3.0-or-later
#include "HttpAdminServer.h"

#include "McpEndpoint.h"

#include <net/AdminChannel.h>
#include <util/Json.h> // json::escape — the one escaper for every JSON this server emits

#include <httplib.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fl {

struct HttpAdminServer::Impl {
    AdminChannel& channel;
    ServerConfig::HttpAdminConfig cfg;
    ILogger& log;
    std::vector<httpadmin::TokenGrant> tokens;
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<uint16_t> port{0};

    // MCP (#601). Null until enableMcp(); its routes are installed only when it exists, so a server
    // with [ai.mcp] disabled serves no MCP path at all rather than one that refuses everything.
    ServerConfig::McpConfig mcpCfg;
    std::unique_ptr<McpEndpoint> mcp;

    const IClock* clock{&SystemClock::instance()};
    // The server's start instant, shared with every other frontend rather than captured here (#1048).
    // Deliberately NOT re-pointed by setClock(): it carries the clock it was started on, so a test
    // that injects a manual clock for lockout expiry cannot accidentally make /health subtract two
    // instants from different clocks.
    ServerUptime uptime;

    Impl(AdminChannel& ch, const ServerConfig::HttpAdminConfig& c, ILogger& l, const ServerUptime& up)
        : channel(ch), cfg(c), log(l), uptime(up) {}

    // Resolve a request to the GRANT behind it, applying the per-IP lockout. Returns nullptr and
    // fills `status`/`body` when the request must be refused.
    //
    // MCP needs the grant itself, not just the issuer it implies — the autonomy tier and the token
    // string (for per-token rate limiting) both live on it — and it must inherit exactly this
    // lockout policy rather than growing a second one.
    const httpadmin::TokenGrant* authorizeGrant(const httplib::Request& req, int& status, std::string& body) {
        const std::string ip = req.remote_addr;
        if (channel.lockedOut(ip)) {
            status = 429;
            body = httpadmin::errorJson("too many failed authentications; try again later");
            return nullptr;
        }

        const std::string presented = httpadmin::extractBearer(req.get_header_value("Authorization"));
        const httpadmin::TokenGrant* grant = httpadmin::resolveToken(tokens, presented);
        if (!grant) {
            if (channel.recordAuthResult(ip, /*authenticated=*/false))
                log.log(LogLevel::Warn, __FILE__, __LINE__, "http_admin: IP locked out after repeated auth failures");
            status = 401;
            body = httpadmin::errorJson("unauthorized");
            return nullptr;
        }

        channel.recordAuthResult(ip, /*authenticated=*/true);
        return grant;
    }

    // The REST routes only ever want the issuer, so they keep asking for exactly that.
    std::optional<CommandIssuer> authorize(const httplib::Request& req, int& status, std::string& body) {
        const httpadmin::TokenGrant* grant = authorizeGrant(req, status, body);
        if (!grant)
            return std::nullopt;
        return httpadmin::issuerFor(*grant);
    }

    // Run an admin command as the request's issuer and turn the result into a response. The registry
    // answers a refusal as a plain string, so map that onto 403 rather than reporting success with a
    // body that says "permission denied" — an HTTP client should not have to read prose to find out.
    void dispatchAsResponse(const CommandIssuer& issuer, const std::string& command, httplib::Response& res) {
        const std::string result = channel.dispatch(command, issuer);
        if (result.rfind("permission denied", 0) == 0) {
            res.status = 403;
            res.set_content(httpadmin::errorJson(result), "application/json");
            return;
        }
        if (result.rfind("unknown command", 0) == 0) {
            res.status = 404;
            res.set_content(httpadmin::errorJson(result), "application/json");
            return;
        }
        res.status = 200;
        res.set_content("{\"result\": \"" + json::escape(result) + "\"}", "application/json");
    }

    // A route whose body is already JSON (worldstate / events) is passed through verbatim rather than
    // wrapped in {"result": "..."} — a client asking for world state wants an object, not a string
    // containing one.
    void dispatchRawJson(const CommandIssuer& issuer, const std::string& command, httplib::Response& res) {
        const std::string result = channel.dispatch(command, issuer);
        if (result.empty() || result.front() != '{') {
            // The command declined (no snapshot yet, or a refusal). Report it as an error object so
            // the response is always JSON.
            res.status = result.rfind("permission denied", 0) == 0 ? 403 : 503;
            res.set_content(httpadmin::errorJson(result), "application/json");
            return;
        }
        res.status = 200;
        res.set_content(result, "application/json");
    }

    void installRoutes();
    void installMcpRoutes();

    // The notification stream wakes on a short interval so a stop() is noticed promptly, and only
    // polls the hooks every kSseWakeupsPerPoll of those — a ~1 s effective poll against a snapshot
    // the sim republishes at about that rate, without a listener thread that ignores shutdown for a
    // whole second.
    static constexpr auto kSseWakeupInterval = std::chrono::milliseconds(100);
    static constexpr int kSseWakeupsPerPoll = 10;
};

void HttpAdminServer::Impl::installRoutes() {
    // /health is the liveness probe and is deliberately the ONE route that authenticates nothing and
    // touches nothing shared: a probe that could block on a lock the stalled sim thread holds would
    // report unhealthy exactly when an orchestrator most needs a truthful answer, and a probe that
    // needed a credential could not be configured in a bare k8s httpGet check.
    server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "{\"status\": \"ok\", \"uptime\": %lld}", uptime.seconds());
        res.status = 200;
        res.set_content(buf, "application/json");
    });

    auto guarded = [this](const char* command, bool rawJson) {
        return [this, command, rawJson](const httplib::Request& req, httplib::Response& res) {
            int status = 0;
            std::string body;
            const auto issuer = authorize(req, status, body);
            if (!issuer) {
                res.status = status;
                res.set_content(body, "application/json");
                return;
            }
            if (rawJson)
                dispatchRawJson(*issuer, command, res);
            else
                dispatchAsResponse(*issuer, command, res);
        };
    };

    server.Get("/status", guarded("status", false));
    server.Get("/peers", guarded("peers", false));
    // The #600 read surfaces, fronted verbatim. REST is a frontend over that assembly, not a second
    // one -- which is why these are one line each.
    server.Get("/worldstate", guarded("worldstate", true));

    server.Get("/events", [this](const httplib::Request& req, httplib::Response& res) {
        int status = 0;
        std::string body;
        const auto issuer = authorize(req, status, body);
        if (!issuer) {
            res.status = status;
            res.set_content(body, "application/json");
            return;
        }
        std::string command = "events";
        if (req.has_param("after"))
            command += " " + req.get_param_value("after");
        if (req.has_param("max"))
            command += (req.has_param("after") ? " " : " 0 ") + req.get_param_value("max");
        dispatchRawJson(*issuer, command, res);
    });

    // Mutating routes. Each maps its JSON body onto the admin command an operator would type, so the
    // capability check, the argument validation and the enqueueSimCallback marshalling all happen in
    // exactly one place -- the command -- rather than being re-implemented per transport.
    auto mutating = [this](auto&& buildCommand) {
        return [this, buildCommand](const httplib::Request& req, httplib::Response& res) {
            int status = 0;
            std::string body;
            const auto issuer = authorize(req, status, body);
            if (!issuer) {
                res.status = status;
                res.set_content(body, "application/json");
                return;
            }
            std::string command;
            std::string err;
            if (!buildCommand(req.body, command, err)) {
                res.status = 400;
                res.set_content(httpadmin::errorJson(err), "application/json");
                return;
            }
            dispatchAsResponse(*issuer, command, res);
        };
    };

    server.Post("/kick", mutating([](const std::string& body, std::string& cmd, std::string& err) {
                    const auto peer = httpadmin::jsonNumberField(body, "peer");
                    if (!peer || *peer < 0) {
                        err = "expected {\"peer\": <id>}";
                        return false;
                    }
                    cmd = "kick " + std::to_string(static_cast<long long>(*peer));
                    return true;
                }));

    for (const char* route : {"/ban", "/unban"}) {
        const std::string verb = std::string(route).substr(1);
        server.Post(route, mutating([verb](const std::string& body, std::string& cmd, std::string& err) {
                        const auto ip = httpadmin::jsonStringField(body, "ip");
                        if (!ip || ip->empty()) {
                            err = "expected {\"ip\": \"<address>\"}";
                            return false;
                        }
                        // The address goes to the command unparsed; `ban`/`unban` already normalise
                        // and validate it, and re-validating here would be a second rule to drift.
                        cmd = verb + " " + *ip;
                        return true;
                    }));
    }

    server.Post("/shutdown", mutating([](const std::string& body, std::string& cmd, std::string& err) {
                    (void)err;
                    cmd = "shutdown";
                    if (const auto in = httpadmin::jsonNumberField(body, "in"); in && *in >= 0)
                        cmd += " --in " + std::to_string(static_cast<long long>(*in));
                    if (const auto reason = httpadmin::jsonStringField(body, "reason"); reason && !reason->empty())
                        cmd += " --reason " + *reason;
                    // A REST caller has already expressed intent by POSTing to /shutdown; requiring a
                    // second confirmation flag it cannot meaningfully supply would just mean every
                    // client hardcodes --force.
                    cmd += " --force";
                    return true;
                }));

    if (mcp)
        installMcpRoutes();

    server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        res.status = 500;
        res.set_content("{\"error\": \"internal error\"}", "application/json");
    });
}

// The MCP frontend (#601): one Streamable HTTP endpoint, POST for calls and GET for notifications.
// Both authenticate through the SAME authorizeGrant the REST routes use, so the per-IP lockout,
// the token table and the capability model are shared rather than mirrored.
void HttpAdminServer::Impl::installMcpRoutes() {
    server.Post(mcpCfg.path, [this](const httplib::Request& req, httplib::Response& res) {
        int status = 0;
        std::string body;
        const httpadmin::TokenGrant* grant = authorizeGrant(req, status, body);
        if (!grant) {
            res.status = status;
            res.set_content(body, "application/json");
            return;
        }
        const auto outcome = mcp->handle(req.body, *grant, req.get_header_value("Mcp-Session-Id"));
        res.status = outcome.httpStatus;
        if (!outcome.newSessionId.empty())
            res.set_header("Mcp-Session-Id", outcome.newSessionId);
        // Echo the revision we implement on every response; 2025-06-18 has clients send it back on
        // subsequent requests, and a client that never sees it cannot.
        res.set_header("MCP-Protocol-Version", std::string(mcp::kProtocolRevision));
        if (outcome.body.empty())
            res.set_content("", "application/json"); // a notification: accepted, no body
        else
            res.set_content(outcome.body, "application/json");
    });

    server.Get(mcpCfg.path, [this](const httplib::Request& req, httplib::Response& res) {
        int status = 0;
        std::string body;
        const httpadmin::TokenGrant* grant = authorizeGrant(req, status, body);
        if (!grant) {
            res.status = status;
            res.set_content(body, "application/json");
            return;
        }
        const std::string sessionId = req.get_header_value("Mcp-Session-Id");
        if (!mcp->sessionExists(sessionId)) {
            // No anonymous streams: a notification stream belongs to an initialized session, or it
            // is a socket held open by something that never handshook.
            res.status = 404;
            res.set_content(httpadmin::errorJson("no such MCP session; call initialize first"), "application/json");
            return;
        }
        res.set_header("MCP-Protocol-Version", std::string(mcp::kProtocolRevision));
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, sessionId](std::size_t /*offset*/, httplib::DataSink& sink) {
                // Poll rather than push: the sim publishes a world snapshot about once a second and
                // has no business knowing an HTTP stream exists. Waking on this interval keeps the
                // seam a std::function returning a tick, not a subscription the sim has to maintain.
                for (int i = 0; i < kSseWakeupsPerPoll && running.load(std::memory_order_relaxed); ++i) {
                    std::this_thread::sleep_for(kSseWakeupInterval);
                    if (!sink.is_writable())
                        return false;
                }
                if (!running.load(std::memory_order_relaxed))
                    return false;
                for (const std::string& frame : mcp->pollNotifications(sessionId)) {
                    const std::string chunk = "data: " + frame + "\n\n";
                    if (!sink.write(chunk.data(), chunk.size()))
                        return false;
                }
                // An SSE comment doubles as the keep-alive: it costs two bytes and stops a proxy
                // from reaping an idle stream on a quiet server.
                static constexpr std::string_view kKeepAlive = ":\n\n";
                return sink.write(kKeepAlive.data(), kKeepAlive.size());
            },
            [this, sessionId](bool) { mcp->dropSession(sessionId); });
    });
}

HttpAdminServer::HttpAdminServer(AdminChannel& channel, const ServerConfig::HttpAdminConfig& cfg, ILogger& log,
                                 const ServerUptime& uptime)
    : m_impl(std::make_unique<Impl>(channel, cfg, log, uptime)) {}

HttpAdminServer::~HttpAdminServer() {
    stop();
}

void HttpAdminServer::enableMcp(const ServerConfig::McpConfig& cfg, McpHooks hooks) {
    m_impl->mcpCfg = cfg;
    // The SAME channel the REST routes use: MCP is a second protocol on one listener with one
    // credential table, not a sixth auth surface an operator would have to know to check.
    m_impl->mcp = std::make_unique<McpEndpoint>(m_impl->channel, cfg, m_impl->log, std::move(hooks));
    m_impl->mcp->setClock(*m_impl->clock);
}

bool HttpAdminServer::start() {
    std::string err;
    if (!httpadmin::buildTokenTable(m_impl->cfg, m_impl->mcpCfg.autonomy, m_impl->tokens, err)) {
        m_impl->log.log(LogLevel::Error, __FILE__, __LINE__, ("http_admin: " + err).c_str());
        return false;
    }
    if (m_impl->tokens.empty()) {
        // The config parser already refuses this combination; belt and braces, because the class is
        // constructible from a hand-built config in a test or a future caller.
        m_impl->log.log(LogLevel::Error, __FILE__, __LINE__,
                        "http_admin: no tokens configured; refusing to start an unauthenticated admin API");
        return false;
    }

    m_impl->installRoutes();

    // Port 0 means "let the OS choose", which needs cpp-httplib's other entry point: bind_to_port
    // returns a BOOL, so using it for an ephemeral bind reports a bound port of 1 and every client
    // then connects to the wrong place.
    int bound = 0;
    if (m_impl->cfg.port == 0) {
        bound = m_impl->server.bind_to_any_port(m_impl->cfg.bindAddress.c_str());
    } else if (m_impl->server.bind_to_port(m_impl->cfg.bindAddress.c_str(), m_impl->cfg.port)) {
        bound = m_impl->cfg.port;
    }
    if (bound <= 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "http_admin: bind failed on %s:%u", m_impl->cfg.bindAddress.c_str(),
                      m_impl->cfg.port);
        m_impl->log.log(LogLevel::Error, __FILE__, __LINE__, buf);
        return false;
    }
    m_impl->port.store(static_cast<uint16_t>(bound), std::memory_order_relaxed);

    m_impl->running.store(true, std::memory_order_relaxed);
    m_impl->thread = std::thread([this] { m_impl->server.listen_after_bind(); });
    // Block until the listener is actually accepting. Without this, start() can return before the
    // thread has entered its accept loop, so an immediate request is refused and an immediate stop()
    // can miss the run flag entirely and hang joining a thread that never started listening.
    m_impl->server.wait_until_ready();

    char buf[192];
    std::snprintf(buf, sizeof(buf), "http_admin: listening on http://%s:%d (%zu token(s))",
                  m_impl->cfg.bindAddress.c_str(), bound, m_impl->tokens.size());
    m_impl->log.log(LogLevel::Info, __FILE__, __LINE__, buf);

    if (m_impl->mcp) {
        // Name the allowlist size explicitly: an operator reading "MCP enabled" and assuming their
        // agent can act is the misreading this line exists to prevent.
        char mbuf[256];
        std::snprintf(mbuf, sizeof(mbuf),
                      "http_admin: MCP surface on %s (revision %.*s, default autonomy %s, %zu allowlisted command(s))",
                      m_impl->mcpCfg.path.c_str(), static_cast<int>(mcp::kProtocolRevision.size()),
                      mcp::kProtocolRevision.data(), m_impl->mcpCfg.autonomy.c_str(), m_impl->mcpCfg.allowlist.size());
        m_impl->log.log(LogLevel::Info, __FILE__, __LINE__, mbuf);
    }
    return true;
}

void HttpAdminServer::stop() {
    if (!m_impl->running.exchange(false))
        return;
    m_impl->server.stop();
    if (m_impl->thread.joinable())
        m_impl->thread.join();
}

uint16_t HttpAdminServer::boundPort() const noexcept {
    return m_impl->port.load(std::memory_order_relaxed);
}

void HttpAdminServer::setClock(const IClock& clock) {
    m_impl->clock = &clock;
    // Forward to MCP too, whichever order the two setters were called in — otherwise a test that
    // enables MCP first ends up with a rate limiter on the real clock and no way to advance it.
    if (m_impl->mcp)
        m_impl->mcp->setClock(clock);
}

} // namespace fl
