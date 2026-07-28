// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <IClock.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// MCP (Model Context Protocol) surface for fl-server — the PURE half (#601).
//
// Everything here is framing, parsing and authorization policy: no sockets, no registry, no world.
// The rule the codebase already follows for `fl::rcon::decodePacket` and `fl::httpadmin`, for the
// same two reasons — a security decision should be unit-testable without opening a file descriptor,
// and a parser that eats untrusted bytes should be fuzzable without a network (fuzz/fuzz_mcp.cpp).
//
// MCP is the SECOND frontend on the #233 HTTP listener, not a new server: plan #1036 D2 (one HTTP
// substrate) and D3 (bearer tokens map onto the #945 capability vocabulary). A tool call resolves to
// a CommandIssuer and goes through the same permission-checked CommandRegistry::dispatch the ENet
// admin channel, RCON, the console and REST all use. The autonomy tier below is an ADDITIONAL gate
// in front of that, never a replacement: a token may be allowed to act and still be refused by the
// command's capability mask.

namespace fl::mcp {

// The dated protocol revision this server implements. Pinned deliberately, and bumping it is a
// deliberate act rather than a dependency drifting underneath us: 2025-06-18 is the revision that
// carries the three things #601 needs — Streamable HTTP (SSE-only transport is deprecated),
// structured tool output (`outputSchema` + `structuredContent`), and resource subscriptions.
//
// It is also the revision that REMOVED JSON-RPC batching, which is why a batch request here is an
// error rather than an unimplemented feature. See docs/ai-architecture.md §4.
inline constexpr std::string_view kProtocolRevision = "2025-06-18";

inline constexpr std::string_view kServerName = "fighters-legacy/fl-server";

// ---------------------------------------------------------------------------
// Autonomy tiers (D3)
// ---------------------------------------------------------------------------
//
// Ordered, and the order is load-bearing: a tool declares the MINIMUM tier that may call it and the
// check is a >= comparison, so adding a tier later does not mean revisiting every tool.
enum class Autonomy : uint8_t {
    Observe = 0,   // read-only: world_state, events. THE DEFAULT for a token that says nothing.
    Recommend = 1, // may additionally validate proposals (submit_mission) without them taking effect
    Act = 2,       // may additionally run allowlisted admin commands
};

[[nodiscard]] std::optional<Autonomy> parseAutonomy(std::string_view s) noexcept;
[[nodiscard]] std::string_view autonomyName(Autonomy a) noexcept;

// ---------------------------------------------------------------------------
// A depth-aware JSON member scanner
// ---------------------------------------------------------------------------
//
// NOT a JSON parser, and deliberately not: the engine's standing answer (engine/perf/JsonScan.h,
// fl::httpadmin) is that a full parser is a dependency this does not need. But MCP params are nested
// one level (`params.arguments.command`), and httpadmin's flat `findKey` — a bare substring search —
// would happily match a key inside a nested object or, worse, inside a STRING VALUE an attacker
// controls. So this one tracks brace/bracket depth and skips over strings and their escapes, and
// returns the raw value span at exactly one object level.
//
// Fails closed everywhere: anything unexpected yields an empty span or nullopt, never a guess.

// Raw value span for `key` at the top level of the object `obj` (which must start at its '{'), or
// an empty span. The span covers the value only, with surrounding whitespace trimmed.
[[nodiscard]] std::string_view objectMember(std::string_view obj, std::string_view key) noexcept;

// Decode a raw span as a JSON string (unescaping), an integer, or a bool. Bounded: a string longer
// than `maxLen` is refused rather than returned, so an over-long field cannot be used to make the
// server allocate on demand.
//
// A caller that accepts a DOCUMENT (submit_mission's YAML) raises the bound and checks the raw span
// itself first, so an over-long argument is reported as over-long instead of as absent — the two
// caps disagreeing, and the misleading error that produced, is what the size test caught.
inline constexpr std::size_t kMaxStringValue = 64 * 1024;
[[nodiscard]] std::optional<std::string> stringValue(std::string_view span, std::size_t maxLen = kMaxStringValue);
[[nodiscard]] std::optional<long long> intValue(std::string_view span) noexcept;
[[nodiscard]] std::optional<bool> boolValue(std::string_view span) noexcept;

// True when the span is a JSON object (`{...}`). Used to refuse `params` that is an array or scalar
// before any member lookup pretends to succeed on it.
[[nodiscard]] bool isObject(std::string_view span) noexcept;

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 envelope
// ---------------------------------------------------------------------------

// JSON-RPC 2.0 reserved codes. MCP adds no codes of its own at this revision; a tool that fails
// reports it in the RESULT (`isError: true`), not as a protocol error — the distinction matters
// because a model is meant to see and reason about a tool failure, but a protocol error means the
// exchange itself is broken.
enum class RpcError : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    Internal = -32603,
};

struct Request {
    // The raw `id` token exactly as it arrived (`1`, `"abc"`), echoed verbatim into the response so
    // a client that used a string id gets a string id back. Empty for a notification.
    std::string id;
    std::string method;
    std::string params; // raw object span, empty when absent
    bool isNotification{false};
};

// Parse one JSON-RPC request. Returns false and fills `errorBody` with a complete, sendable
// JSON-RPC error response on malformed input. A batch (a top-level array) is rejected with
// InvalidRequest — batching was removed in this revision.
[[nodiscard]] bool parseRequest(std::string_view body, Request& out, std::string& errorBody);

// Response builders. `id` is the raw token from Request::id; an empty id emits `null`, which is what
// JSON-RPC requires for an error that could not be attributed to a request.
[[nodiscard]] std::string resultResponse(std::string_view id, std::string_view resultJson);
[[nodiscard]] std::string errorResponse(std::string_view id, RpcError code, std::string_view message);

// A `tools/call` result. `structured` is emitted as `structuredContent` alongside the text block, so
// a client gets the machine-readable form and the human-readable one from a single call.
[[nodiscard]] std::string toolResult(std::string_view text, std::string_view structured, bool isError);

// ---------------------------------------------------------------------------
// Tool catalog — data, so tools/list and the dispatcher cannot disagree
// ---------------------------------------------------------------------------

struct ToolDesc {
    std::string_view name;
    std::string_view title;
    std::string_view description;
    std::string_view inputSchema;  // JSON Schema object
    std::string_view outputSchema; // JSON Schema object; empty = unstructured result only
    Autonomy minTier;
};

[[nodiscard]] std::span<const ToolDesc> toolCatalog() noexcept;
[[nodiscard]] const ToolDesc* findTool(std::string_view name) noexcept;

// The two resources exposed for subscription: the world-state snapshot and the match event log.
struct ResourceDesc {
    std::string_view uri;
    std::string_view name;
    std::string_view description;
    std::string_view mimeType;
};
[[nodiscard]] std::span<const ResourceDesc> resourceCatalog() noexcept;
[[nodiscard]] const ResourceDesc* findResource(std::string_view uri) noexcept;

// ---------------------------------------------------------------------------
// admin_command allowlist
// ---------------------------------------------------------------------------

// The first whitespace-delimited token of a command line — the verb the allowlist names. Returns an
// empty view for an empty or whitespace-only line.
[[nodiscard]] std::string_view commandVerb(std::string_view line) noexcept;

// True when the line's verb appears in `allowlist`. An EMPTY allowlist permits NOTHING: an operator
// who enables MCP without listing commands has not implicitly authorized every command, and the
// alternative reading ("empty means unrestricted") is the kind of default that turns a config typo
// into a remote shell.
[[nodiscard]] bool commandAllowed(std::span<const std::string> allowlist, std::string_view line) noexcept;

// ---------------------------------------------------------------------------
// Per-token rate limiting
// ---------------------------------------------------------------------------
//
// Fixed window per token, with an injectable clock (the AuthTracker precedent). Keyed by token
// rather than by IP because the token is the thing being granted authority: one agent behind a
// proxy and one agent per pod must be limited the same way.
class RateLimiter {
  public:
    explicit RateLimiter(int maxPerMinute) : m_maxPerMinute(maxPerMinute) {}

    void setClock(const IClock& clock) noexcept {
        m_clock = &clock;
    }

    // Records a call and reports whether it is allowed. A non-positive limit disables the limiter.
    [[nodiscard]] bool allow(const std::string& token);

    // Drop windows not touched for two full windows, so a server that has seen many short-lived
    // tokens does not keep their counters forever.
    void pruneExpired();

    [[nodiscard]] std::size_t trackedTokens() const;

  private:
    struct Window {
        std::chrono::steady_clock::time_point start{};
        int count{0};
    };
    int m_maxPerMinute;
    const IClock* m_clock{&SystemClock::instance()};
    mutable std::mutex m_mutex;
    std::vector<std::pair<std::string, Window>> m_windows;
};

} // namespace fl::mcp
