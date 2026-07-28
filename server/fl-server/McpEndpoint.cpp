// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpEndpoint.h"

#include <console/CommandRegistry.h>
#include <mission/MissionValidator.h>
#include <net/WorldStateJson.h> // jsonEscape — one escaper for every JSON this server emits

#include "Version.h" // FL_VERSION_STRING — reported to a client in initialize's serverInfo

#include <chrono>
#include <cstdio>
#include <random>

namespace fl {

namespace {

// A refusal the CommandRegistry answers as prose. Both the REST frontend and this one key on the
// same two prefixes, because the registry's contract is the string, not an error code.
[[nodiscard]] bool isPermissionDenied(const std::string& s) {
    return s.rfind("permission denied", 0) == 0;
}
[[nodiscard]] bool isUnknownCommand(const std::string& s) {
    return s.rfind("unknown command", 0) == 0;
}

// Cap what a single tool call may be asked to chew on. A mission document is YAML from an agent that
// may itself be reading attacker-influenced text, so the size bound belongs here rather than in the
// parser's error path.
constexpr std::size_t kMaxMissionYamlBytes = 512 * 1024;
constexpr long long kMaxEventsPerCall = 1000;

} // namespace

McpEndpoint::McpEndpoint(const CommandRegistry& registry, const ServerConfig::McpConfig& cfg, ILogger& log,
                         McpHooks hooks)
    : m_registry(registry), m_cfg(cfg), m_log(log), m_hooks(std::move(hooks)), m_limiter(cfg.rateLimitPerMin) {}

void McpEndpoint::setClock(const IClock& clock) {
    m_clock = &clock;
    m_limiter.setClock(clock);
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

std::string McpEndpoint::newSessionId() {
    // Random, not sequential: a session id is a capability for the notification stream, so a client
    // must not be able to guess the next one. Same reasoning as LocalServer's session token.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const uint64_t hi = rng();
    const uint64_t lo = rng();
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf);
}

void McpEndpoint::evictIfNeeded() {
    // Caller holds m_sessionMutex.
    const auto cap = static_cast<std::size_t>(m_cfg.maxSessions > 0 ? m_cfg.maxSessions : 1);
    bool evicted = false;
    while (m_sessions.size() > cap) {
        auto oldest = m_sessions.begin();
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
            if (it->second.lastSeen < oldest->second.lastSeen)
                oldest = it;
        m_sessions.erase(oldest);
        evicted = true;
    }
    // Worth saying out loud: an evicted session's notification stream stops, and from the client's
    // side that is indistinguishable from the server having gone away. An operator seeing this
    // either has max_sessions set too low or has something opening sessions and abandoning them,
    // and neither is visible anywhere else.
    if (evicted) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "mcp: session cap (%d) reached; evicted the idlest session", m_cfg.maxSessions);
        m_log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
    }
}

bool McpEndpoint::sessionExists(std::string_view id) const {
    std::lock_guard<std::mutex> lk(m_sessionMutex);
    return m_sessions.find(std::string(id)) != m_sessions.end();
}

void McpEndpoint::dropSession(std::string_view id) {
    std::lock_guard<std::mutex> lk(m_sessionMutex);
    m_sessions.erase(std::string(id));
}

std::size_t McpEndpoint::sessionCount() const {
    std::lock_guard<std::mutex> lk(m_sessionMutex);
    return m_sessions.size();
}

std::vector<std::string> McpEndpoint::pollNotifications(std::string_view id) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(m_sessionMutex);
    auto it = m_sessions.find(std::string(id));
    if (it == m_sessions.end())
        return out;
    Session& s = it->second;
    s.lastSeen = m_clock->now();

    // Notify that the RESOURCE CHANGED, not what it now says. A subscriber decides whether it wants
    // to spend a read; pushing a multi-thousand-entity snapshot at every subscriber every second
    // would make the notification stream the most expensive thing on the server.
    if (s.subscribedWorldState && m_hooks.worldStateTick) {
        const uint64_t tick = m_hooks.worldStateTick();
        if (tick != 0 && tick != s.lastWorldTick) {
            s.lastWorldTick = tick;
            out.emplace_back(R"({"jsonrpc": "2.0", "method": "notifications/resources/updated", )"
                             R"("params": {"uri": "fl://world_state"}})");
        }
    }
    if (s.subscribedEvents && m_hooks.matchEventSeq) {
        const uint64_t seq = m_hooks.matchEventSeq();
        if (seq != s.lastEventSeq) {
            s.lastEventSeq = seq;
            out.emplace_back(R"({"jsonrpc": "2.0", "method": "notifications/resources/updated", )"
                             R"("params": {"uri": "fl://events"}})");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

std::string McpEndpoint::readResource(std::string_view uri, const httpadmin::TokenGrant& grant, bool& ok) {
    ok = false;
    const CommandIssuer issuer = httpadmin::issuerFor(grant);
    std::string result;
    if (uri == "fl://world_state")
        result = m_registry.dispatch("worldstate", issuer);
    else if (uri == "fl://events")
        result = m_registry.dispatch("events", issuer);
    else
        return "unknown resource";

    if (result.empty() || result.front() != '{')
        return result.empty() ? "no snapshot available yet" : result;
    ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

std::string McpEndpoint::handleToolCall(std::string_view params, const httpadmin::TokenGrant& grant,
                                        std::string_view id) {
    const auto name = mcp::stringValue(mcp::objectMember(params, "name"));
    if (!name)
        return mcp::errorResponse(id, mcp::RpcError::InvalidParams, "missing tool \"name\"");

    const mcp::ToolDesc* tool = mcp::findTool(*name);
    if (!tool)
        return mcp::errorResponse(id, mcp::RpcError::InvalidParams, "unknown tool '" + *name + "'");

    // Tier gate. A refusal here is a PROTOCOL error, not a tool result: the token was never allowed
    // to reach this tool, which is a fact about the caller's authority rather than an outcome the
    // model should reason about and retry.
    if (static_cast<uint8_t>(grant.autonomy) < static_cast<uint8_t>(tool->minTier)) {
        return mcp::errorResponse(id, mcp::RpcError::InvalidRequest,
                                  "tool '" + *name + "' requires autonomy tier '" +
                                      std::string(mcp::autonomyName(tool->minTier)) + "'; this token is '" +
                                      std::string(mcp::autonomyName(grant.autonomy)) + "'");
    }

    const std::string_view args = mcp::objectMember(params, "arguments");
    const CommandIssuer issuer = httpadmin::issuerFor(grant);

    // Audit BEFORE dispatch, and record the attempt whatever its outcome: a refused agent action is
    // exactly the thing an operator reading the audit trail wants to see.
    if (m_hooks.auditAgentAction)
        m_hooks.auditAgentAction(*name, issuer, args.empty() ? std::string_view{"{}"} : args);

    if (*name == "world_state") {
        bool ok = false;
        // The text is the document either way (or the refusal); only the STRUCTURED half is withheld
        // when the read failed, because a failure message is not the schema the tool advertises.
        const std::string json = readResource("fl://world_state", grant, ok);
        return mcp::resultResponse(id, mcp::toolResult(json, ok ? json : std::string_view{}, !ok));
    }

    if (*name == "events") {
        std::string command = "events";
        long long after = 0;
        long long max = 0;
        if (const auto v = mcp::intValue(mcp::objectMember(args, "after")); v && *v >= 0)
            after = *v;
        if (const auto v = mcp::intValue(mcp::objectMember(args, "max")); v && *v > 0)
            max = *v > kMaxEventsPerCall ? kMaxEventsPerCall : *v;
        // The command takes positionals, so `max` alone still has to pass an `after` of 0.
        if (after > 0 || max > 0) {
            command += " " + std::to_string(after);
            if (max > 0)
                command += " " + std::to_string(max);
        }
        const std::string json = m_registry.dispatch(command, issuer);
        const bool ok = !json.empty() && json.front() == '{';
        return mcp::resultResponse(id, mcp::toolResult(json, ok ? json : std::string_view{}, !ok));
    }

    if (*name == "submit_mission") {
        const std::string_view yamlSpan = mcp::objectMember(args, "yaml");
        if (yamlSpan.empty())
            return mcp::errorResponse(id, mcp::RpcError::InvalidParams, "missing \"yaml\" argument");
        // Bound the RAW span, before decoding: an over-long document is refused without allocating
        // for it, and — the reason this is checked here rather than after — it is reported as
        // over-long rather than as absent, which is what a bound applied inside stringValue would
        // have said.
        if (yamlSpan.size() > kMaxMissionYamlBytes)
            return mcp::resultResponse(id, mcp::toolResult("mission document too large", {}, true));
        const auto yaml = mcp::stringValue(yamlSpan, kMaxMissionYamlBytes);
        if (!yaml)
            return mcp::errorResponse(id, mcp::RpcError::InvalidParams, "\"yaml\" is not a valid JSON string");

        const MissionValidationResult v = validateMission(*yaml);
        std::string structured = "{\"ok\": ";
        structured += v.ok ? "true" : "false";
        structured += ", \"errors\": [";
        for (std::size_t i = 0; i < v.errors.size(); ++i)
            structured += (i ? ", \"" : "\"") + jsonEscape(v.errors[i]) + "\"";
        structured += "], \"warnings\": [";
        for (std::size_t i = 0; i < v.warnings.size(); ++i)
            structured += (i ? ", \"" : "\"") + jsonEscape(v.warnings[i]) + "\"";
        structured += "]}";

        std::string text = v.ok ? "mission is valid" : "mission is invalid";
        for (const std::string& e : v.errors)
            text += "\nerror: " + e;
        for (const std::string& w : v.warnings)
            text += "\nwarning: " + w;
        // A schema failure is a TOOL result, not a protocol error: it is the answer the agent asked
        // for, and it is the one thing it can act on to produce a better document.
        return mcp::resultResponse(id, mcp::toolResult(text, structured, !v.ok));
    }

    if (*name == "admin_command") {
        const auto command = mcp::stringValue(mcp::objectMember(args, "command"));
        if (!command || command->empty())
            return mcp::errorResponse(id, mcp::RpcError::InvalidParams, "missing \"command\" argument");
        if (!mcp::commandAllowed(m_cfg.allowlist, *command)) {
            return mcp::resultResponse(id, mcp::toolResult("command '" + std::string(mcp::commandVerb(*command)) +
                                                               "' is not on this server's MCP allowlist",
                                                           {}, true));
        }
        // Through the SAME permission-checked dispatch every other frontend uses. The allowlist above
        // narrows what MCP may attempt; the capability mask inside still decides what it may do.
        const std::string result = m_registry.dispatch(*command, issuer);
        const bool failed = isPermissionDenied(result) || isUnknownCommand(result);
        return mcp::resultResponse(id, mcp::toolResult(result, "{\"result\": \"" + jsonEscape(result) + "\"}", failed));
    }

    return mcp::errorResponse(id, mcp::RpcError::Internal, "tool '" + *name + "' has no handler");
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

McpEndpoint::RpcOutcome McpEndpoint::handle(std::string_view body, const httpadmin::TokenGrant& grant,
                                            std::string_view sessionId) {
    RpcOutcome out;

    mcp::Request req;
    std::string parseError;
    if (!mcp::parseRequest(body, req, parseError)) {
        out.body = parseError;
        out.httpStatus = 400;
        return out;
    }

    // Rate limit per token, before any work. A notification is limited too: an unbounded stream of
    // them is the cheapest way to make a server busy.
    if (!m_limiter.allow(grant.token)) {
        if (req.isNotification) {
            out.httpStatus = 429;
            return out;
        }
        out.body = mcp::errorResponse(req.id, mcp::RpcError::Internal, "rate limit exceeded");
        out.httpStatus = 429;
        return out;
    }

    if (req.method == "initialize") {
        std::string id = newSessionId();
        {
            std::lock_guard<std::mutex> lk(m_sessionMutex);
            Session s;
            s.token = grant.token;
            s.lastSeen = m_clock->now();
            m_sessions.emplace(id, std::move(s));
            evictIfNeeded();
        }
        // Answer with the revision WE implement. The spec's negotiation is "offer yours, the server
        // states its own"; refusing an unfamiliar client revision outright would break clients that
        // are perfectly able to speak ours.
        std::string result = "{\"protocolVersion\": \"";
        result += mcp::kProtocolRevision;
        result += "\", \"capabilities\": {\"tools\": {\"listChanged\": false}, "
                  "\"resources\": {\"subscribe\": true, \"listChanged\": false}}, "
                  "\"serverInfo\": {\"name\": \"";
        result += mcp::kServerName;
        result += "\", \"version\": \"";
        result += jsonEscape(FL_VERSION_STRING);
        result += "\"}}";
        out.body = mcp::resultResponse(req.id, result);
        out.newSessionId = std::move(id);
        return out;
    }

    // Everything past initialize needs a live session, so a caller cannot skip the handshake and
    // start calling tools with nothing but a bearer token.
    //
    // The session is also BOUND to the token that opened it. Authority is re-derived from the
    // presented grant on every request, so a borrowed session id cannot escalate anything — but a
    // session is a handle to a subscription state, and one token quietly driving another's stream is
    // a confusion worth refusing outright rather than reasoning about later.
    bool sessionOk = false;
    if (!sessionId.empty()) {
        std::lock_guard<std::mutex> lk(m_sessionMutex);
        if (auto it = m_sessions.find(std::string(sessionId)); it != m_sessions.end()) {
            sessionOk = it->second.token == grant.token;
            if (sessionOk)
                it->second.lastSeen = m_clock->now();
        }
    }
    if (req.method != "ping" && !sessionOk) {
        if (req.isNotification) {
            out.httpStatus = 404;
            return out;
        }
        out.body = mcp::errorResponse(req.id, mcp::RpcError::InvalidRequest,
                                      "no MCP session for this token; call initialize first (Mcp-Session-Id header)");
        out.httpStatus = 404;
        return out;
    }

    if (req.isNotification) {
        // notifications/initialized and friends. Nothing to answer, and per spec the transport
        // reports acceptance rather than a body.
        out.httpStatus = 202;
        return out;
    }

    if (req.method == "ping") {
        out.body = mcp::resultResponse(req.id, "{}");
        return out;
    }

    if (req.method == "tools/list") {
        std::string result = "{\"tools\": [";
        bool first = true;
        for (const mcp::ToolDesc& t : mcp::toolCatalog()) {
            // A tool the caller's tier can never reach is omitted rather than advertised — an agent
            // should not spend a turn discovering it is not allowed to do something.
            if (static_cast<uint8_t>(grant.autonomy) < static_cast<uint8_t>(t.minTier))
                continue;
            if (!first)
                result += ", ";
            first = false;
            result += "{\"name\": \"";
            result += t.name;
            result += "\", \"title\": \"";
            result += t.title;
            result += "\", \"description\": \"";
            result += jsonEscape(t.description);
            result += "\", \"inputSchema\": ";
            result += t.inputSchema;
            if (!t.outputSchema.empty()) {
                result += ", \"outputSchema\": ";
                result += t.outputSchema;
            }
            result += "}";
        }
        result += "]}";
        out.body = mcp::resultResponse(req.id, result);
        return out;
    }

    if (req.method == "tools/call") {
        out.body = handleToolCall(req.params, grant, req.id);
        return out;
    }

    if (req.method == "resources/list") {
        std::string result = "{\"resources\": [";
        bool first = true;
        for (const mcp::ResourceDesc& r : mcp::resourceCatalog()) {
            if (!first)
                result += ", ";
            first = false;
            result += "{\"uri\": \"";
            result += r.uri;
            result += "\", \"name\": \"";
            result += r.name;
            result += "\", \"description\": \"";
            result += jsonEscape(r.description);
            result += "\", \"mimeType\": \"";
            result += r.mimeType;
            result += "\"}";
        }
        result += "]}";
        out.body = mcp::resultResponse(req.id, result);
        return out;
    }

    if (req.method == "resources/read") {
        const auto uri = mcp::stringValue(mcp::objectMember(req.params, "uri"));
        if (!uri)
            out.body = mcp::errorResponse(req.id, mcp::RpcError::InvalidParams, "missing \"uri\"");
        else if (!mcp::findResource(*uri))
            out.body = mcp::errorResponse(req.id, mcp::RpcError::InvalidParams, "unknown resource '" + *uri + "'");
        else {
            bool ok = false;
            const std::string text = readResource(*uri, grant, ok);
            if (!ok) {
                out.body = mcp::errorResponse(req.id, mcp::RpcError::Internal, text);
            } else {
                std::string result = "{\"contents\": [{\"uri\": \"";
                result += *uri;
                result += "\", \"mimeType\": \"application/json\", \"text\": \"";
                result += jsonEscape(text);
                result += "\"}]}";
                out.body = mcp::resultResponse(req.id, result);
            }
        }
        return out;
    }

    if (req.method == "resources/subscribe" || req.method == "resources/unsubscribe") {
        const auto uri = mcp::stringValue(mcp::objectMember(req.params, "uri"));
        if (!uri || !mcp::findResource(*uri)) {
            out.body = mcp::errorResponse(req.id, mcp::RpcError::InvalidParams, "unknown or missing \"uri\"");
            return out;
        }
        const bool on = req.method == "resources/subscribe";
        {
            std::lock_guard<std::mutex> lk(m_sessionMutex);
            if (auto it = m_sessions.find(std::string(sessionId)); it != m_sessions.end()) {
                if (*uri == "fl://world_state")
                    it->second.subscribedWorldState = on;
                else
                    it->second.subscribedEvents = on;
            }
        }
        out.body = mcp::resultResponse(req.id, "{}");
        return out;
    }

    out.body = mcp::errorResponse(req.id, mcp::RpcError::MethodNotFound, "unknown method '" + req.method + "'");
    return out;
}

} // namespace fl
