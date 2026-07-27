// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "server_config.h"

#include <ILogger.h>
#include <net/AuthTracker.h>
#include <net/Capability.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace fl {
class CommandRegistry;
class CommandShell;
} // namespace fl

// ---------------------------------------------------------------------------
// Pure logic (no sockets). Everything that makes a security decision lives here so it is unit-
// testable without opening a file descriptor and fuzzable without a network — the rule
// fl::rcon::decodePacket already follows.
// ---------------------------------------------------------------------------
namespace fl::httpadmin {

// A resolved token row: the bearer secret and the authority it confers.
struct TokenGrant {
    std::string token;
    CapabilityMask caps{0};
    uint16_t factionIndex{PeerAuthority::kNoFactionBinding};
    std::string role;
};

// Build the token table from config. A row naming an unknown role preset is REJECTED rather than
// silently downgraded: an operator who typos "moderatr" must not end up with a token that
// authenticates and then refuses everything, because that reads as the server being broken.
// Returns false and fills `error` on the first bad row.
[[nodiscard]] bool buildTokenTable(const ServerConfig::HttpAdminConfig& cfg, std::vector<TokenGrant>& out,
                                   std::string& error);

// Extract the credential from an `Authorization: Bearer <token>` header value. Returns an empty
// string when the header is absent, malformed, or uses a different scheme. The scheme match is
// case-insensitive because RFC 7235 says it is.
[[nodiscard]] std::string extractBearer(std::string_view authHeader);

// Constant-time equality over the whole of both strings. A plain `==` on a secret leaks its length
// and its matching prefix through timing; the ENet admin channel already compares its operator
// password this way and the REST tokens are the same kind of secret.
[[nodiscard]] bool constantTimeEquals(std::string_view a, std::string_view b) noexcept;

// Find the grant for a presented token. Every row is compared (no early exit) so the lookup does not
// leak which row matched, or how many rows exist, through timing. Returns nullptr when nothing
// matches or when `presented` is empty.
[[nodiscard]] const TokenGrant* resolveToken(const std::vector<TokenGrant>& table, std::string_view presented);

// Turn a grant into the issuer the permission-checked dispatch expects. A REST request is not a peer,
// so peerId is kIssuerNoPeer. NOTE: a default-constructed CommandIssuer is ADMIN, so this must always
// be used rather than filling one field at a time and hoping.
[[nodiscard]] CommandIssuer issuerFor(const TokenGrant& grant) noexcept;

// Minimal JSON field readers for the small request bodies (`{"peer": 3}`, `{"ip": "1.2.3.4"}`).
// These are scanners over an expected shape, not a JSON parser — the same standard
// engine/perf/JsonScan.h sets, and the same reason: a full parser is a dependency this does not
// need. They fail closed: anything unexpected yields nullopt rather than a guess.
[[nodiscard]] std::optional<double> jsonNumberField(std::string_view json, std::string_view key);
[[nodiscard]] std::optional<std::string> jsonStringField(std::string_view json, std::string_view key);

// A JSON error body, escaped. Used for every non-2xx response so a client never has to distinguish
// a JSON body from a bare string.
[[nodiscard]] std::string errorJson(std::string_view message);

} // namespace fl::httpadmin

namespace fl {

// The embedded REST admin API (#233).
//
// Every route resolves the request's bearer token to a CommandIssuer and then calls the SAME
// permission-checked CommandRegistry::dispatch the ENet admin channel uses. It is the fourth frontend
// over one command substrate, not a parallel admin implementation — so a capability added to a
// command is enforced here for free, and a command that does not exist cannot be reached over HTTP.
//
// Threading: cpp-httplib owns a listener thread and a thread per request. dispatch() is const and
// mutating handlers enqueue through GameLoop::enqueueSimCallback, which is the same contract
// RconServer relies on. /health deliberately touches NOTHING that the sim thread can hold, so it
// still answers while the sim is stalled — which is the entire point of a liveness probe.
class HttpAdminServer {
  public:
    HttpAdminServer(const CommandRegistry& registry, const ServerConfig::HttpAdminConfig& cfg, ILogger& log,
                    CommandShell* shell = nullptr);
    ~HttpAdminServer();

    // Bind and launch the listener thread. Returns false on a bad token table or a bind failure; the
    // server then continues without the REST API rather than refusing to boot.
    [[nodiscard]] bool start();

    // Stop the listener and join its thread. Safe if start() was never called or failed.
    void stop();

    // Clear the per-IP auth lockout for the HTTP channel. Thread-safe, any thread (the admin_unlock
    // command reaches it from the sim thread).
    bool clearLockout(const std::string& ip);

    // Lockout state for the HTTP channel, for admin_auth_status. Thread-safe.
    [[nodiscard]] AuthLockoutSummary getAuthSummary();

    // The bound port. Meaningful after a successful start(); 0 otherwise. A configured port of 0 asks
    // the OS to choose one, which is what a test needs in order not to fight over a fixed port.
    [[nodiscard]] uint16_t boundPort() const noexcept;

    // Inject a clock for deterministic lockout expiry in tests. Call before start().
    void setClock(const IClock& clock);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
