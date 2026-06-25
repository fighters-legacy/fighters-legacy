// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "server_config.h"
#include <ILogger.h>
#include <cstdint>
#include <memory>
#include <net/AuthTracker.h>
#include <string>
#include <string_view>
#include <vector>

namespace fl {
class CommandRegistry;
class CommandShell;
} // namespace fl

// ---------------------------------------------------------------------------
// Source Engine RCON wire-protocol helpers (pure logic, no sockets).
// All functions are fully unit-testable without opening any file descriptors.
// ---------------------------------------------------------------------------
namespace fl::rcon {

// Maximum body bytes per response packet before splitting is required.
// Source Engine RCON clients expect packets ≤ 4096 bytes total;
// 10 bytes of header + NUL pair leaves 4086 bytes for body text.
constexpr int kMaxBodyPerPacket = 4086;

// RCON packet type constants (Source Engine RCON protocol).
constexpr int32_t kTypeResponseValue = 0; // server→client: command response
constexpr int32_t kTypeAuth = 3;          // client→server: authentication
constexpr int32_t kTypeAuthResponse = 2;  // server→client: auth result
constexpr int32_t kTypeExecCommand = 2;   // client→server: execute command

struct RconPacket {
    int32_t id = 0;
    int32_t type = 0;
    std::string body;
};

// Encode a packet to wire bytes (4-byte LE size + id + type + body + NUL NUL).
std::vector<uint8_t> encodePacket(int32_t id, int32_t type, std::string_view body);

// Decode one packet from buf[0..len). Returns bytes consumed (>0) on success,
// 0 if more data is needed, -1 if the packet is malformed.
int decodePacket(const uint8_t* buf, int len, RconPacket& out);

// Split a response body into chunks of at most kMaxBodyPerPacket bytes.
// Always returns at least one element (may be empty string for empty input).
std::vector<std::string> splitResponse(std::string_view body);

} // namespace fl::rcon

// ---------------------------------------------------------------------------
// RconServer -- TCP RCON listener (Source Engine RCON protocol).
// Runs a background I/O thread; the caller's CommandRegistry is invoked
// directly from that thread (safe: dispatch() is const and all mutating
// handlers enqueue work through the thread-safe GameLoop::enqueueSimCallback).
// ---------------------------------------------------------------------------
namespace fl {

class RconServer {
  public:
    RconServer(const CommandRegistry& registry, const ServerConfig::RconConfig& cfg, ILogger& log,
               CommandShell* shell = nullptr);
    ~RconServer();

    // Bind the TCP listen socket and launch the background I/O thread.
    // Returns false on bind failure; server continues running without RCON.
    bool start();

    // Signal the background thread to exit and join it. Closes all sockets.
    // Safe to call even if start() was never called or returned false.
    void stop();

    // Clear the RCON auth lockout for ip. Thread-safe; may be called from any thread.
    // Returns true if a lockout was active and was cleared.
    bool clearLockout(const std::string& ip);

    // Read the current RCON auth lockout state. Thread-safe; acquires the internal mutex.
    fl::AuthLockoutSummary getRconAuthSummary();

    // Override the clock used for drain-deadline timing and auth-lockout expiry.
    // Must be called before start(). The clock must outlive this server.
    // Propagates to the internal AuthTracker.
    void setClock(const IClock& clock);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
