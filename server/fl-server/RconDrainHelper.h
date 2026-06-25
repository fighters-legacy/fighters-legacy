// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <vector>

namespace fl {
class CommandShell;
} // namespace fl

namespace fl::rcon {

// Per-client drain state. Platform-agnostic: holds no socket descriptor.
// ClientState in RconServer.cpp publicly inherits this struct.
// Tests construct DrainClientInfo directly without opening any file descriptors.
struct DrainClientInfo {
    bool connected = false; // true = socket is live; set false on disconnect
    bool hasPendingDrain = false;
    int drainMark = 0;
    int32_t drainPacketId = 0;
    std::chrono::steady_clock::time_point drainDeadline{};
    std::vector<uint8_t> sendBuf; // outbound byte stream
};

// Scan clients for pending drains whose deadline has passed and fire each once:
// reads lines from shell since drainMark, encodes them as SERVERDATA_RESPONSE_VALUE
// packets, and appends encoded bytes into client.sendBuf.
// shell == nullptr → no-op.
// Extracted from ioLoop() to enable deadline testing without real TCP sockets.
void checkAndFireDrains(std::vector<DrainClientInfo*>& clients, std::chrono::steady_clock::time_point now,
                        fl::CommandShell* shell);

} // namespace fl::rcon
