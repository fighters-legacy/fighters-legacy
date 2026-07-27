// SPDX-License-Identifier: GPL-3.0-or-later
#include "HttpAdminServer.h"

#include <console/CommandRegistry.h>
#include <console/CommandShell.h>
#include <net/WorldStateJson.h> // jsonEscape — one escaper for every JSON this server emits

#include <httplib.h>

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace fl::httpadmin {

bool buildTokenTable(const ServerConfig::HttpAdminConfig& cfg, std::vector<TokenGrant>& out, std::string& error) {
    out.clear();
    for (const ServerConfig::HttpAdminToken& row : cfg.tokens) {
        const auto caps = parseRolePreset(row.role);
        if (!caps) {
            error = "unknown role preset '" + row.role + "' (expected admin|moderator|gm|faction_leader)";
            return false;
        }
        TokenGrant g;
        g.token = row.token;
        g.caps = *caps;
        g.role = row.role;
        g.factionIndex = row.faction >= 0 && row.faction <= 0xFFFE ? static_cast<uint16_t>(row.faction)
                                                                   : PeerAuthority::kNoFactionBinding;
        out.push_back(std::move(g));
    }
    return true;
}

std::string extractBearer(std::string_view authHeader) {
    constexpr std::string_view kScheme = "bearer";
    if (authHeader.size() <= kScheme.size())
        return {};
    for (std::size_t i = 0; i < kScheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(authHeader[i])) != kScheme[i])
            return {};
    std::size_t pos = kScheme.size();
    // Require at least one space, then skip any run of them.
    if (authHeader[pos] != ' ')
        return {};
    while (pos < authHeader.size() && authHeader[pos] == ' ')
        ++pos;
    return std::string(authHeader.substr(pos));
}

bool constantTimeEquals(std::string_view a, std::string_view b) noexcept {
    // Fold the length difference into the accumulator instead of returning early, so the comparison
    // takes the same shape whatever the presented credential looks like.
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

const TokenGrant* resolveToken(const std::vector<TokenGrant>& table, std::string_view presented) {
    if (presented.empty())
        return nullptr;
    const TokenGrant* found = nullptr;
    for (const TokenGrant& g : table)
        if (constantTimeEquals(g.token, presented))
            found = &g; // no break: every row is compared, so timing does not reveal which matched
    return found;
}

CommandIssuer issuerFor(const TokenGrant& grant) noexcept {
    // Built in one expression on purpose. A default-constructed CommandIssuer is ADMIN, so any code
    // path that fills fields one at a time can forget `caps` and silently grant everything.
    return CommandIssuer{kIssuerNoPeer, grant.caps, grant.factionIndex};
}

namespace {

// Locate `"key"` at an object level we are willing to read: these bodies are one flat object, so
// anything nested is not a field this endpoint accepts.
[[nodiscard]] std::size_t findKey(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t k = json.find(needle);
    if (k == std::string_view::npos)
        return std::string_view::npos;
    std::size_t p = k + needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r'))
        ++p;
    if (p >= json.size() || json[p] != ':')
        return std::string_view::npos;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r'))
        ++p;
    return p;
}

} // namespace

std::optional<double> jsonNumberField(std::string_view json, std::string_view key) {
    const std::size_t p = findKey(json, key);
    if (p == std::string_view::npos || p >= json.size())
        return std::nullopt;
    // strtod, not from_chars: Apple Clang has no floating-point from_chars (the JsonScan.h rule).
    const std::string tail(json.substr(p, 64));
    char* end = nullptr;
    const double v = std::strtod(tail.c_str(), &end);
    if (end == tail.c_str())
        return std::nullopt;
    return v;
}

std::optional<std::string> jsonStringField(std::string_view json, std::string_view key) {
    const std::size_t p = findKey(json, key);
    if (p == std::string_view::npos || p >= json.size() || json[p] != '"')
        return std::nullopt;
    std::string out;
    // Bound the field: a body is untrusted, and an unterminated quote must not make us scan forever
    // or return a megabyte because someone sent one.
    constexpr std::size_t kMaxField = 512;
    for (std::size_t i = p + 1; i < json.size() && out.size() <= kMaxField; ++i) {
        const char c = json[i];
        if (c == '"')
            return out;
        if (c == '\\') {
            if (++i >= json.size())
                return std::nullopt;
            switch (json[i]) {
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            default:
                out += json[i];
                break;
            }
            continue;
        }
        out += c;
    }
    return std::nullopt; // unterminated or over-long
}

std::string errorJson(std::string_view message) {
    return "{\"error\": \"" + jsonEscape(message) + "\"}";
}

} // namespace fl::httpadmin

namespace fl {

struct HttpAdminServer::Impl {
    const CommandRegistry& registry;
    ServerConfig::HttpAdminConfig cfg;
    ILogger& log;
    // Held but not yet read. RCON needs it because a mutating command's real confirmation lands on
    // the shell ring a tick after dispatch returns, and RCON can keep sending packets; HTTP answers
    // the synchronous ack and closes the exchange. Kept because #601's MCP surface shares this
    // listener and will want the deferred output.
    CommandShell* shell;

    std::vector<httpadmin::TokenGrant> tokens;
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<uint16_t> port{0};

    std::mutex authMutex;
    AuthTracker auth;
    const IClock* clock{&SystemClock::instance()};
    std::chrono::steady_clock::time_point startedAt{};

    Impl(const CommandRegistry& reg, const ServerConfig::HttpAdminConfig& c, ILogger& l, CommandShell* sh)
        : registry(reg), cfg(c), log(l), shell(sh), auth(c.maxAuthFailures, c.lockoutSeconds) {}

    // Resolve a request to an issuer, applying the per-IP lockout. Returns nullopt and fills
    // `status`/`body` when the request must be refused.
    std::optional<CommandIssuer> authorize(const httplib::Request& req, int& status, std::string& body) {
        const std::string ip = req.remote_addr;
        {
            std::lock_guard<std::mutex> lk(authMutex);
            if (auth.isLockedOut(ip)) {
                status = 429;
                body = httpadmin::errorJson("too many failed authentications; try again later");
                return std::nullopt;
            }
        }

        const std::string presented = httpadmin::extractBearer(req.get_header_value("Authorization"));
        const httpadmin::TokenGrant* grant = httpadmin::resolveToken(tokens, presented);
        if (!grant) {
            bool nowLocked = false;
            {
                std::lock_guard<std::mutex> lk(authMutex);
                nowLocked = auth.recordFailure(ip);
            }
            if (nowLocked)
                log.log(LogLevel::Warn, __FILE__, __LINE__, "http_admin: IP locked out after repeated auth failures");
            status = 401;
            body = httpadmin::errorJson("unauthorized");
            return std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lk(authMutex);
            auth.recordSuccess(ip);
        }
        return httpadmin::issuerFor(*grant);
    }

    // Run an admin command as the request's issuer and turn the result into a response. The registry
    // answers a refusal as a plain string, so map that onto 403 rather than reporting success with a
    // body that says "permission denied" — an HTTP client should not have to read prose to find out.
    void dispatchAsResponse(const CommandIssuer& issuer, const std::string& command, httplib::Response& res) {
        const std::string result = registry.dispatch(command, issuer);
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
        res.set_content("{\"result\": \"" + jsonEscape(result) + "\"}", "application/json");
    }

    // A route whose body is already JSON (worldstate / events) is passed through verbatim rather than
    // wrapped in {"result": "..."} — a client asking for world state wants an object, not a string
    // containing one.
    void dispatchRawJson(const CommandIssuer& issuer, const std::string& command, httplib::Response& res) {
        const std::string result = registry.dispatch(command, issuer);
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
};

void HttpAdminServer::Impl::installRoutes() {
    // /health is the liveness probe and is deliberately the ONE route that authenticates nothing and
    // touches nothing shared: a probe that could block on a lock the stalled sim thread holds would
    // report unhealthy exactly when an orchestrator most needs a truthful answer, and a probe that
    // needed a credential could not be configured in a bare k8s httpGet check.
    server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        const auto up = std::chrono::duration_cast<std::chrono::seconds>(clock->now() - startedAt).count();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "{\"status\": \"ok\", \"uptime\": %lld}", static_cast<long long>(up));
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

    server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        res.status = 500;
        res.set_content("{\"error\": \"internal error\"}", "application/json");
    });
}

HttpAdminServer::HttpAdminServer(const CommandRegistry& registry, const ServerConfig::HttpAdminConfig& cfg,
                                 ILogger& log, CommandShell* shell)
    : m_impl(std::make_unique<Impl>(registry, cfg, log, shell)) {}

HttpAdminServer::~HttpAdminServer() {
    stop();
}

bool HttpAdminServer::start() {
    std::string err;
    if (!httpadmin::buildTokenTable(m_impl->cfg, m_impl->tokens, err)) {
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

    m_impl->startedAt = m_impl->clock->now();
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
    return true;
}

void HttpAdminServer::stop() {
    if (!m_impl->running.exchange(false))
        return;
    m_impl->server.stop();
    if (m_impl->thread.joinable())
        m_impl->thread.join();
}

bool HttpAdminServer::clearLockout(const std::string& ip) {
    std::lock_guard<std::mutex> lk(m_impl->authMutex);
    // Report whether a lockout was actually active, so `admin_unlock` can tell the operator it did
    // something rather than always claiming success (the RconServer::clearLockout contract).
    const bool wasLockedOut = m_impl->auth.isLockedOut(ip);
    m_impl->auth.clearLockout(ip);
    return wasLockedOut;
}

AuthLockoutSummary HttpAdminServer::getAuthSummary() {
    std::lock_guard<std::mutex> lk(m_impl->authMutex);
    AuthLockoutSummary s;
    s.activeCount = m_impl->auth.lockedOutCount();
    s.threshold = m_impl->auth.maxFailures();
    s.entries = m_impl->auth.failureSummary();
    return s;
}

uint16_t HttpAdminServer::boundPort() const noexcept {
    return m_impl->port.load(std::memory_order_relaxed);
}

void HttpAdminServer::setClock(const IClock& clock) {
    m_impl->clock = &clock;
    m_impl->auth.setClock(clock);
}

} // namespace fl
