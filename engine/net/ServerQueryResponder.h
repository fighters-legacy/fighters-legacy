// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SocketCompat.h> // socket_t / WsaGuard / the winsock include order (#1256)

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fl {

class ILogger;

// Server side of the server-info query protocol (#997). Owns a dedicated UDP socket + a background
// thread that answers MsgServerQuery with MsgServerInfo. A dedicated port + thread (not the 50 ms admin
// loop) keeps RTT measurement accurate and works under both transports (GNS has no raw-datagram hook).
// Static info (name/mode/mission) and dynamic info (players/shutdown/passworded) are set from the main
// loop under a mutex. Anti-amplification: datagrams shorter than sizeof(MsgServerQuery) are dropped;
// per-source-IP + global rate limiting bound reflection abuse.
class ServerQueryResponder {
  public:
    struct StaticInfo {
        std::string name;
        std::string modeId;
        std::string mission;
        uint16_t gamePort{0};
        uint8_t maxPlayers{0};
        uint8_t gameModeFlags{0}; // static capability bits (campaign/mission/sandbox/passworded)
        // Appended at the tail (#1074) so existing brace-initialization at call sites keeps working —
        // the same tail-append discipline the wire structs follow, for the same reason.
        std::string buildVersion; // advertised so a browser shows the build without connecting
    };
    struct DynamicInfo {
        uint8_t playerCount{0};
        bool shuttingDown{false};
        uint16_t shutdownSeconds{0};
    };

    ServerQueryResponder(uint16_t queryPort, ILogger& log);
    ~ServerQueryResponder();

    ServerQueryResponder(const ServerQueryResponder&) = delete;
    ServerQueryResponder& operator=(const ServerQueryResponder&) = delete;

    // Bind + launch the responder thread. Returns false if the socket could not be bound.
    bool start();
    void stop();

    void setStaticInfo(StaticInfo info);
    void setDynamicInfo(const DynamicInfo& info);

    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }

  private:
    void run();

    uint16_t m_port{0};
    ILogger* m_log{nullptr};
    WsaGuard m_wsa; // #1256: OS-refcounted, so taking a reference is free
    socket_t m_sock{kInvalidSocket};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::mutex m_infoMutex;
    StaticInfo m_static;
    DynamicInfo m_dynamic;
};

} // namespace fl
