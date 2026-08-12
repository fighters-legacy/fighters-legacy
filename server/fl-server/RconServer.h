// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "server_config.h"
#include <ILogger.h>
#include <cstdint>
#include <memory>
#include <net/AdminChannel.h>
#include <string>
#include <string_view>
#include <vector>

namespace fl {
class AdminChannel;
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

// Encode drained shell lines as RESPONSE_VALUE packets for `packetId`: joined with newlines, split at
// kMaxBodyPerPacket, and terminated with an empty sentinel packet when the body needed more than one.
// Returns no bytes for no lines. Pure logic -- the DEADLINE that decides when to call this belongs to
// AdminChannel now (#1079); this is the half that is genuinely RCON's.
std::vector<uint8_t> encodeDrainPackets(int32_t packetId, const std::vector<std::string>& lines);

} // namespace fl::rcon

// ---------------------------------------------------------------------------
// RconServer -- TCP RCON listener (Source Engine RCON protocol).
// Runs a background I/O thread and drives its AdminChannel from that thread: dispatch, the per-IP
// lockout and the deferred shell drain all live on the channel, which is safe because dispatch is
// const and mutating handlers enqueue through GameLoop::enqueueSimCallback (#1079).
// ---------------------------------------------------------------------------
namespace fl {

class RconServer {
  public:
    // `channel` is this frontend's AdminChannel: it carries the dispatcher, the lockout parameters and
    // the shell tap, and must outlive the server. cfg is still needed for the port and the password --
    // the credential LADDER is per-transport by design; only the bookkeeping around it is shared.
    RconServer(AdminChannel& channel, const ServerConfig::RconConfig& cfg, ILogger& log);
    ~RconServer();

    // Bind the TCP listen socket and launch the background I/O thread.
    // Returns false on bind failure; server continues running without RCON.
    bool start();

    // Signal the background thread to exit and join it. Closes all sockets.
    // Safe to call even if start() was never called or returned false.
    void stop();

    // Override the clock used for the poll-timeout arithmetic. Must be called before start(); the clock
    // must outlive this server. Lockout expiry and drain deadlines run on the channel's clock, which the
    // channel takes at construction -- there is no second clock to keep in step any more.
    void setClock(const IClock& clock);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
