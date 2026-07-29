// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

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
    RoleDenied,          // requested role not allowed by the server (#857)
    MissingRequiredPack, // client lacks a server-required content pack (#872)
    EntitlementRequired, // premium content requires an entitlement token (RFC #871)
    MatchFull,           // every team in the current game mode is at capacity (#522)
    BadPassword,         // the server requires a join password and the client's was missing/wrong (#998)
    ServerFull,          // the server world is at its entity cap and could not spawn an airframe (#1049)
    NoAirframe,          // the server has no spawnable player aircraft type configured (#1049)
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
    case SessionFailure::RoleDenied:
        return "The server denied the requested role.";
    case SessionFailure::MissingRequiredPack:
        return "You are missing a content pack this server requires.";
    case SessionFailure::EntitlementRequired:
        return "This server requires premium content you do not own.";
    case SessionFailure::MatchFull:
        return "All teams are full.";
    case SessionFailure::BadPassword:
        return "Incorrect server password.";
    case SessionFailure::ServerFull:
        return "The server world is full. Try again shortly.";
    case SessionFailure::NoAirframe:
        return "This server has no aircraft available.";
    }
    return "";
}

// Stable i18n key for a failure (#358), e.g. "session.server_spawn_failed". Paired with
// sessionFailureMessage(): the client looks the key up in the active locale and falls back to the
// English message when the key is absent. None returns "" (never displayed). Every enumerator has a
// key; tests/test_session_i18n.cpp asserts each key is present + non-empty in locale/en/ui.toml.
inline const char* sessionFailureKey(SessionFailure f) {
    switch (f) {
    case SessionFailure::None:
        return "";
    case SessionFailure::ServerSpawnFailed:
        return "ui.session.server_spawn_failed";
    case SessionFailure::ServerBindFailed:
        return "ui.session.server_bind_failed";
    case SessionFailure::ServerStartTimeout:
        return "ui.session.server_start_timeout";
    case SessionFailure::ServerStartHang:
        return "ui.session.server_start_hang";
    case SessionFailure::VersionMismatch:
        return "ui.session.version_mismatch";
    case SessionFailure::Banned:
        return "ui.session.banned";
    case SessionFailure::AccessDenied:
        return "ui.session.access_denied";
    case SessionFailure::RateLimited:
        return "ui.session.rate_limited";
    case SessionFailure::TooManyConnections:
        return "ui.session.too_many_connections";
    case SessionFailure::ConnectionRefused:
        return "ui.session.connection_refused";
    case SessionFailure::ConnectTimeout:
        return "ui.session.connect_timeout";
    case SessionFailure::RoleDenied:
        return "ui.session.role_denied";
    case SessionFailure::MissingRequiredPack:
        return "ui.session.missing_required_pack";
    case SessionFailure::EntitlementRequired:
        return "ui.session.entitlement_required";
    case SessionFailure::MatchFull:
        return "ui.session.match_full";
    case SessionFailure::BadPassword:
        return "ui.session.bad_password";
    case SessionFailure::ServerFull:
        return "ui.session.server_full";
    case SessionFailure::NoAirframe:
        return "ui.session.no_airframe";
    }
    return "";
}

} // namespace fl
