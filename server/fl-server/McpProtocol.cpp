// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpProtocol.h"

#include <net/WorldStateJson.h> // jsonEscape — one escaper for every JSON this server emits

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
// Scanner
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool isWs(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isWs(s[b]))
        ++b;
    while (e > b && isWs(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

// Index just past the string literal starting at `i` (which must be its opening quote), or npos if
// it never terminates. Escapes are skipped as a unit so a `\"` does not end the string.
[[nodiscard]] std::size_t skipString(std::string_view s, std::size_t i) noexcept {
    ++i; // opening quote
    while (i < s.size()) {
        if (s[i] == '\\') {
            i += 2; // the escape and whatever it escapes
            continue;
        }
        if (s[i] == '"')
            return i + 1;
        ++i;
    }
    return std::string_view::npos;
}

// Index just past the JSON value starting at `i`, or npos. Handles the four shapes that can appear
// as a member value; a scalar simply runs to the next delimiter at depth 0.
[[nodiscard]] std::size_t skipValue(std::string_view s, std::size_t i) noexcept {
    if (i >= s.size())
        return std::string_view::npos;
    if (s[i] == '"')
        return skipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                i = skipString(s, i);
                if (i == std::string_view::npos)
                    return std::string_view::npos;
                continue;
            }
            if (c == '{' || c == '[')
                ++depth;
            else if (c == '}' || c == ']') {
                --depth;
                if (depth == 0)
                    return i + 1;
                if (depth < 0)
                    return std::string_view::npos;
            }
            ++i;
        }
        return std::string_view::npos;
    }
    // Scalar: number, true, false, null.
    const std::size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isWs(s[i]))
        ++i;
    return i > start ? i : std::string_view::npos;
}

} // namespace

bool isObject(std::string_view span) noexcept {
    const std::string_view t = trim(span);
    return t.size() >= 2 && t.front() == '{' && t.back() == '}';
}

std::string_view objectMember(std::string_view obj, std::string_view key) noexcept {
    const std::string_view s = trim(obj);
    if (s.empty() || s.front() != '{')
        return {};

    std::size_t i = 1; // past '{'
    while (i < s.size()) {
        while (i < s.size() && (isWs(s[i]) || s[i] == ','))
            ++i;
        if (i >= s.size() || s[i] == '}')
            return {};
        if (s[i] != '"')
            return {}; // a key must be a string; anything else means malformed, so fail closed

        const std::size_t keyStart = i + 1;
        const std::size_t afterKey = skipString(s, i);
        if (afterKey == std::string_view::npos)
            return {};
        // Compare the RAW key bytes. Every key this server looks for is plain ASCII with no escapes,
        // so an escaped spelling of one is not a match we want to honour.
        const std::string_view thisKey = s.substr(keyStart, afterKey - keyStart - 1);

        i = afterKey;
        while (i < s.size() && isWs(s[i]))
            ++i;
        if (i >= s.size() || s[i] != ':')
            return {};
        ++i;
        while (i < s.size() && isWs(s[i]))
            ++i;

        const std::size_t valStart = i;
        const std::size_t valEnd = skipValue(s, i);
        if (valEnd == std::string_view::npos)
            return {};
        if (thisKey == key)
            return trim(s.substr(valStart, valEnd - valStart));
        i = valEnd;
    }
    return {};
}

std::optional<std::string> stringValue(std::string_view span, std::size_t maxLen) {
    const std::string_view s = trim(span);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"')
        return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (out.size() > maxLen)
            return std::nullopt; // over-long: refuse rather than truncate into something plausible
        const char c = s[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i + 1 > s.size() - 1)
            return std::nullopt;
        switch (s[i]) {
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'u': {
            // \uXXXX. Only the BMP, encoded as UTF-8; a lone surrogate becomes U+FFFD rather than
            // producing invalid UTF-8 that some downstream consumer chokes on.
            if (i + 4 >= s.size() - 1)
                return std::nullopt;
            unsigned cp = 0;
            for (int k = 1; k <= 4; ++k) {
                const char h = s[i + static_cast<std::size_t>(k)];
                cp <<= 4;
                if (h >= '0' && h <= '9')
                    cp |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f')
                    cp |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    cp |= static_cast<unsigned>(h - 'A' + 10);
                else
                    return std::nullopt;
            }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0xFFFD;
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            return std::nullopt; // an unknown escape is malformed, not a literal
        }
    }
    return out;
}

std::optional<long long> intValue(std::string_view span) noexcept {
    const std::string_view s = trim(span);
    if (s.empty())
        return std::nullopt;
    long long v = 0;
    const auto* first = s.data();
    const auto* last = s.data() + s.size();
    const auto res = std::from_chars(first, last, v);
    if (res.ec != std::errc{} || res.ptr != last)
        return std::nullopt;
    return v;
}

std::optional<bool> boolValue(std::string_view span) noexcept {
    const std::string_view s = trim(span);
    if (s == "true")
        return true;
    if (s == "false")
        return false;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

bool parseRequest(std::string_view body, Request& out, std::string& errorBody) {
    const std::string_view s = trim(body);
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
    if (!isObject(s)) {
        errorBody = errorResponse({}, RpcError::ParseError, "request must be a JSON object");
        return false;
    }

    const std::string_view ver = objectMember(s, "jsonrpc");
    const auto verStr = stringValue(ver);
    if (!verStr || *verStr != "2.0") {
        errorBody = errorResponse({}, RpcError::InvalidRequest, "expected \"jsonrpc\": \"2.0\"");
        return false;
    }

    const std::string_view idSpan = objectMember(s, "id");
    out.id.assign(idSpan);
    // No id, or an explicit null id, means a notification: no response is emitted at all.
    out.isNotification = out.id.empty() || out.id == "null";
    if (out.isNotification)
        out.id.clear();

    const auto method = stringValue(objectMember(s, "method"));
    if (!method || method->empty()) {
        errorBody = errorResponse(out.id, RpcError::InvalidRequest, "missing \"method\"");
        return false;
    }
    out.method = *method;

    const std::string_view params = objectMember(s, "params");
    if (!params.empty() && !isObject(params)) {
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
    s += jsonEscape(message);
    s += "\"}}";
    return s;
}

std::string toolResult(std::string_view text, std::string_view structured, bool isError) {
    std::string s = "{\"content\": [{\"type\": \"text\", \"text\": \"";
    s += jsonEscape(text);
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
    while (b < line.size() && isWs(line[b]))
        ++b;
    std::size_t e = b;
    while (e < line.size() && !isWs(line[e]))
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
