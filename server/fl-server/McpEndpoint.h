// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "HttpAdminServer.h" // httpadmin::TokenGrant, McpHooks
#include "McpProtocol.h"
#include "server_config.h"

#include <ILogger.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fl {
class CommandRegistry;
}

// The MCP method dispatcher (#601) — everything between "an authenticated request arrived" and "here
// is the JSON to send back".
//
// Deliberately free of httplib: the transport is HttpAdminServer's business, and keeping this class
// transport-free is what lets test_mcp_protocol drive every method, every tier refusal and every
// session path without binding a port. The same reason fl::rcon's logic is separable from its socket.
//
// THE ONE INVARIANT WORTH STATING: this class never decides what a command is permitted to do. It
// decides which TOOLS a token's autonomy tier may reach, and then hands the work to
// CommandRegistry::dispatch(line, issuer), which applies the #945 capability mask exactly as it does
// for the console, RCON, the ENet admin channel and REST. An `act`-tier moderator token is refused
// `shutdown` by the capability check, not by anything here — and that refusal is the proof MCP is a
// frontend rather than a parallel admin path.

namespace fl {

class McpEndpoint {
  public:
    McpEndpoint(const CommandRegistry& registry, const ServerConfig::McpConfig& cfg, ILogger& log, McpHooks hooks);

    // Inject a clock for deterministic rate-limit and session-expiry tests. Call before use.
    void setClock(const IClock& clock);

    struct RpcOutcome {
        // The response JSON. EMPTY means "send no body": the message was a JSON-RPC notification,
        // which per spec is answered with 202 Accepted and nothing else.
        std::string body;
        // Non-empty only on a successful `initialize`, where it is the id to hand back in the
        // Mcp-Session-Id response header.
        std::string newSessionId;
        int httpStatus{200};
    };

    // Handle one JSON-RPC message from an already-authenticated caller. `sessionId` is the
    // Mcp-Session-Id header value, empty when absent.
    [[nodiscard]] RpcOutcome handle(std::string_view body, const httpadmin::TokenGrant& grant,
                                    std::string_view sessionId);

    // --- notification stream (GET <path>) ---

    // True when the id names a live session. A GET with an unknown session id is refused rather
    // than opening an anonymous stream, so a stream always belongs to an initialized session.
    [[nodiscard]] bool sessionExists(std::string_view id) const;

    // Frames to emit on the SSE stream for this session, empty when nothing changed. Each frame is
    // a complete JSON-RPC notification object; the caller wraps it in SSE framing.
    [[nodiscard]] std::vector<std::string> pollNotifications(std::string_view id);

    void dropSession(std::string_view id);

    [[nodiscard]] std::size_t sessionCount() const;

  private:
    struct Session {
        std::string token; // which grant opened it; a session never outlives its token's authority
        std::chrono::steady_clock::time_point lastSeen{};
        bool subscribedWorldState{false};
        bool subscribedEvents{false};
        uint64_t lastWorldTick{0};
        uint64_t lastEventSeq{0};
    };

    [[nodiscard]] std::string handleToolCall(std::string_view params, const httpadmin::TokenGrant& grant,
                                             std::string_view id);
    [[nodiscard]] std::string readResource(std::string_view uri, const httpadmin::TokenGrant& grant, bool& ok);
    [[nodiscard]] std::string newSessionId();
    void evictIfNeeded();

    const CommandRegistry& m_registry;
    ServerConfig::McpConfig m_cfg;
    ILogger& m_log;
    McpHooks m_hooks;
    const IClock* m_clock{&SystemClock::instance()};
    mcp::RateLimiter m_limiter;

    mutable std::mutex m_sessionMutex;
    std::unordered_map<std::string, Session> m_sessions;
};

} // namespace fl
