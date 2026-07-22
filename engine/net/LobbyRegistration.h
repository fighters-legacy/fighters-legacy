// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "IHttpClient.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace fl {

class ILogger;

// Static + dynamic config for lobby registration (#143). The static fields are set once at
// configure(); players/mission change over the session via setDynamic().
struct LobbyRegistrationConfig {
    std::string lobbyUrl;        // base, e.g. "https://lobby.fighters-legacy.org"
    std::string name;            // server name
    uint16_t gamePort{0};        // the game port a client connects to
    std::string mode;            // game-mode id
    std::string mission;         // current mission/map
    int maxPlayers{0};           // capacity
    int heartbeatS{30};          // POST interval; [5, 300]
    bool visibilityPublic{true}; // false ("private") => never registers
};

// Registers a dedicated server with a lobby over HTTP (#143): POST <lobbyUrl>/v1/servers on a heartbeat
// interval, DELETE on shutdown. The lobby keys the entry on the request's source IP + the advertised
// port, so the server never sends its own host (NAT is out of scope for v1). Injected IHttpClient — the
// engine stays HAL-clean; fl-server owns the concrete backend. Main-loop only, never the sim thread.
//
// Failure handling: a failed POST logs Warn once and backs off exponentially (heartbeat * 2^n, capped at
// 5 minutes), recovering to the normal cadence on the next success.
class LobbyRegistration : public IHttpClientHandler {
  public:
    LobbyRegistration(IHttpClient& http, ILogger& log);

    // Set the static config. Registration is disabled (a no-op) when visibilityPublic is false or the
    // lobby url is empty.
    void configure(const LobbyRegistrationConfig& cfg);

    // Update the mutable fields advertised on the next heartbeat.
    void setDynamic(int players, const std::string& mission);

    // Call once per main-loop frame: sends a heartbeat POST when due (and not already in flight).
    void tick();

    // Send a best-effort DELETE to drop the entry immediately (server shutdown). Idempotent.
    void deregister();

    [[nodiscard]] bool enabled() const noexcept {
        return m_enabled;
    }
    // The JSON body of the last POST (test hook).
    [[nodiscard]] const std::string& lastBody() const noexcept {
        return m_lastBody;
    }

    void setClock(const IClock& clock) noexcept {
        m_clock = &clock;
    }

    // IHttpClientHandler.
    bool onHttpData(HttpRequestId, const void*, std::size_t) override {
        return true;
    }
    void onHttpComplete(HttpRequestId id, HttpStatus status, long httpCode, const char* errorMsg) override;

  private:
    void postHeartbeat();
    std::string buildBody() const;
    std::string endpoint() const; // <lobbyUrl>/v1/servers

    IHttpClient& m_http;
    ILogger& m_log;
    const IClock* m_clock{&SystemClock::instance()};

    LobbyRegistrationConfig m_cfg;
    int m_players{0};
    bool m_enabled{false};

    HttpRequestId m_reqId{0};
    bool m_havePosted{false};
    std::chrono::steady_clock::time_point m_lastPost{};
    int m_backoffMultiplier{1}; // grows on failure, resets to 1 on success
    bool m_warnedFailure{false};
    std::string m_lastBody;
};

} // namespace fl
