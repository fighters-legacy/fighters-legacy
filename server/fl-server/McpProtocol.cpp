// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpProtocol.h"

#include <util/Json.h> // json::escape — the one escaper for every JSON this server emits

#include <array>
#include <cctype>
#include <charconv>

namespace fl::mcp {

// ---------------------------------------------------------------------------
// Autonomy
// ---------------------------------------------------------------------------

std::optional<Autonomy> parseAutonomy(std::string_view s) noexcept {
    if (s == "observe")
        return Autonomy::Observe;
    if (s == "recommend")
        return Autonomy::Recommend;
    if (s == "act")
        return Autonomy::Act;
    return std::nullopt;
}

std::string_view autonomyName(Autonomy a) noexcept {
    switch (a) {
    case Autonomy::Observe:
        return "observe";
    case Autonomy::Recommend:
        return "recommend";
    case Autonomy::Act:
        return "act";
    }
    return "observe";
}

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

bool parseRequest(std::string_view body, Request& out, std::string& errorBody) {
    const std::string_view s = json::trim(body);
    if (s.empty()) {
        errorBody = errorResponse({}, RpcError::ParseError, "empty request body");
        return false;
    }
    if (s.front() == '[') {
        // Batching was removed in 2025-06-18. Say so, rather than reporting a generic parse failure
        // that leaves a client guessing whether its JSON was wrong.
        errorBody = errorResponse({}, RpcError::InvalidRequest,
                                  "JSON-RPC batching is not supported in MCP revision 2025-06-18");
        return false;
    }
    if (!json::isObject(s)) {
        errorBody = errorResponse({}, RpcError::ParseError, "request must be a JSON object");
        return false;
    }

    const std::string_view ver = json::member(s, "jsonrpc");
    const auto verStr = json::stringValue(ver);
    if (!verStr || *verStr != "2.0") {
        errorBody = errorResponse({}, RpcError::InvalidRequest, "expected \"jsonrpc\": \"2.0\"");
        return false;
    }

    const std::string_view idSpan = json::member(s, "id");
    out.id.assign(idSpan);
    // No id, or an explicit null id, means a notification: no response is emitted at all.
    out.isNotification = out.id.empty() || out.id == "null";
    if (out.isNotification)
        out.id.clear();

    const auto method = json::stringValue(json::member(s, "method"));
    if (!method || method->empty()) {
        errorBody = errorResponse(out.id, RpcError::InvalidRequest, "missing \"method\"");
        return false;
    }
    out.method = *method;

    const std::string_view params = json::member(s, "params");
    if (!params.empty() && !json::isObject(params)) {
        errorBody = errorResponse(out.id, RpcError::InvalidParams, "\"params\" must be an object");
        return false;
    }
    out.params.assign(params);
    return true;
}

std::string resultResponse(std::string_view id, std::string_view resultJson) {
    std::string s = "{\"jsonrpc\": \"2.0\", \"id\": ";
    s += id.empty() ? "null" : std::string(id);
    s += ", \"result\": ";
    s += resultJson;
    s += "}";
    return s;
}

std::string errorResponse(std::string_view id, RpcError code, std::string_view message) {
    std::string s = "{\"jsonrpc\": \"2.0\", \"id\": ";
    s += id.empty() ? "null" : std::string(id);
    s += ", \"error\": {\"code\": ";
    s += std::to_string(static_cast<int>(code));
    s += ", \"message\": \"";
    s += json::escape(message);
    s += "\"}}";
    return s;
}

std::string toolResult(std::string_view text, std::string_view structured, bool isError) {
    std::string s = "{\"content\": [{\"type\": \"text\", \"text\": \"";
    s += json::escape(text);
    s += "\"}]";
    if (!structured.empty()) {
        s += ", \"structuredContent\": ";
        s += structured;
    }
    s += ", \"isError\": ";
    s += isError ? "true" : "false";
    s += "}";
    return s;
}

// ---------------------------------------------------------------------------
// Catalogs
// ---------------------------------------------------------------------------

namespace {

// Schemas are string literals rather than generated: they are part of the wire contract, they change
// only when a tool's arguments change, and a golden test compares them so a silent edit fails CI.
constexpr std::string_view kNoArgs = R"({"type": "object", "properties": {}, "additionalProperties": false})";

constexpr std::array<ToolDesc, 4> kTools{{
    {"world_state", "World state",
     "The current authoritative world snapshot: entities, peers, factions and their relationships, "
     "alert levels, mission state and weather. Rebuilt about once per second.",
     kNoArgs,
     // The output schema IS the #600 golden JSON schema, so the schema test that guards REST guards
     // this too and there is no second description of the same document to drift.
     R"({"type": "object", "properties": {"tick": {"type": "integer"}, "entities": {"type": "array"}, )"
     R"("peers": {"type": "array"}, "factions": {"type": "array"}, "mission": {"type": "object"}}})",
     Autonomy::Observe},

    {"events", "Match events",
     "Tail of the append-only match event log: kills, spawns, chat, admin commands, alert changes "
     "and agent actions. Pass `after` to resume from a sequence number; `gap` in the result means "
     "records were dropped before the ones returned.",
     R"({"type": "object", "properties": {"after": {"type": "integer", "minimum": 0}, )"
     R"("max": {"type": "integer", "minimum": 1, "maximum": 1000}}, "additionalProperties": false})",
     R"({"type": "object", "properties": {"events": {"type": "array"}, "nextSeq": {"type": "integer"}, )"
     R"("gap": {"type": "boolean"}}})",
     Autonomy::Observe},

    {"submit_mission", "Submit a mission",
     "Validate a mission YAML document against the engine's own schema. Reports every error and "
     "warning; it does not load the mission.",
     R"({"type": "object", "properties": {"yaml": {"type": "string"}}, "required": ["yaml"], )"
     R"("additionalProperties": false})",
     R"({"type": "object", "properties": {"ok": {"type": "boolean"}, "errors": {"type": "array"}, )"
     R"("warnings": {"type": "array"}}})",
     Autonomy::Recommend},

    {"admin_command", "Run an admin command",
     "Run one server admin command. The command must appear on the server's allowlist AND be "
     "permitted by the calling token's capabilities; both are checked server-side.",
     R"({"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"], )"
     R"("additionalProperties": false})",
     R"({"type": "object", "properties": {"result": {"type": "string"}}})", Autonomy::Act},
}};

constexpr std::array<ResourceDesc, 2> kResources{{
    {"fl://world_state", "World state", "The authoritative world snapshot, updated about once per second.",
     "application/json"},
    {"fl://events", "Match events", "The match event log tail.", "application/json"},
}};

} // namespace

std::span<const ToolDesc> toolCatalog() noexcept {
    return kTools;
}

const ToolDesc* findTool(std::string_view name) noexcept {
    for (const ToolDesc& t : kTools)
        if (t.name == name)
            return &t;
    return nullptr;
}

std::span<const ResourceDesc> resourceCatalog() noexcept {
    return kResources;
}

const ResourceDesc* findResource(std::string_view uri) noexcept {
    for (const ResourceDesc& r : kResources)
        if (r.uri == uri)
            return &r;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Allowlist
// ---------------------------------------------------------------------------

std::string_view commandVerb(std::string_view line) noexcept {
    std::size_t b = 0;
    while (b < line.size() && json::isWs(line[b]))
        ++b;
    std::size_t e = b;
    while (e < line.size() && !json::isWs(line[e]))
        ++e;
    return line.substr(b, e - b);
}

bool commandAllowed(std::span<const std::string> allowlist, std::string_view line) noexcept {
    const std::string_view verb = commandVerb(line);
    if (verb.empty())
        return false;
    for (const std::string& allowed : allowlist)
        if (allowed == verb)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Rate limiting
// ---------------------------------------------------------------------------

bool RateLimiter::allow(const std::string& token) {
    if (m_maxPerMinute <= 0)
        return true;
    const auto now = m_clock->now();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [key, w] : m_windows) {
        if (key != token)
            continue;
        if (now - w.start >= std::chrono::minutes(1)) {
            w.start = now;
            w.count = 0;
        }
        if (w.count >= m_maxPerMinute)
            return false;
        ++w.count;
        return true;
    }
    m_windows.emplace_back(token, Window{now, 1});
    return true;
}

void RateLimiter::pruneExpired() {
    const auto now = m_clock->now();
    std::lock_guard<std::mutex> lk(m_mutex);
    std::erase_if(m_windows, [&](const auto& e) { return now - e.second.start >= std::chrono::minutes(2); });
}

std::size_t RateLimiter::trackedTokens() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_windows.size();
}

} // namespace fl::mcp
