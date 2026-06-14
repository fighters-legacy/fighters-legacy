// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// Typed cross-thread session-failure status. Replaces the prior `std::atomic<const char*>` +
// static-string-literal signaling between the server-start thread, the ENet client handler, and the
// LoadingScreen. A single `std::atomic<SessionFailure>` is lock-free and trivially copyable (no
// string-lifetime concerns, no per-handler char buffer), and every display string is produced by one
// mapping function — the single point the i18n system will localize later (issue #358).
enum class SessionFailure : uint8_t {
    None = 0,
    // Local-server startup (single-player). Set by Game's server thread from LocalServer::StartResult,
    // except ServerStartHang which the LoadingScreen raises when start() never returns.
    ServerSpawnFailed,  // server binary not found
    ServerBindFailed,   // port already in use
    ServerStartTimeout, // LocalServer::start() reported a timeout
    ServerStartHang,    // start() never returned (LoadingScreen start-deadline fallback)
    // Connection. Set by ClientNetEventHandler, except ConnectTimeout from the LoadingScreen deadline.
    VersionMismatch,
    Banned,
    AccessDenied,
    RateLimited,
    TooManyConnections,
    ConnectionRefused, // generic ENet-level rejection before MsgConnectAck
    ConnectTimeout,
};

// English display text for a failure (empty for None). Single mapping point: wrap these in the
// i18n system later without scattering literals across the call sites.
inline const char* sessionFailureMessage(SessionFailure f) {
    switch (f) {
    case SessionFailure::None:
        return "";
    case SessionFailure::ServerSpawnFailed:
        return "Server binary not found.";
    case SessionFailure::ServerBindFailed:
        return "Port already in use.";
    case SessionFailure::ServerStartTimeout:
        return "Server startup timed out.";
    case SessionFailure::ServerStartHang:
        return "Local server failed to start.";
    case SessionFailure::VersionMismatch:
        return "Server version mismatch.";
    case SessionFailure::Banned:
        return "You are banned from this server.";
    case SessionFailure::AccessDenied:
        return "Access denied.";
    case SessionFailure::RateLimited:
        return "Connection rate limit exceeded. Try again later.";
    case SessionFailure::TooManyConnections:
        return "Too many connections from your address.";
    case SessionFailure::ConnectionRefused:
        return "Connection refused by server.";
    case SessionFailure::ConnectTimeout:
        return "Connection timed out.";
    }
    return "";
}
