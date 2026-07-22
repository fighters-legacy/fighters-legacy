// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IHttpClient.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

class ILogger;

// One server as advertised by a lobby (#143). Populated by parseLobbyServerList from the lobby's
// GET /v1/servers JSON. Fields absent in the JSON keep their defaults.
struct LobbyServer {
    std::string name;
    std::string host; // hostname or IP the client connects to
    uint16_t port{0};
    std::string mode;    // game-mode id
    std::string mission; // current mission/map display name
    int players{0};
    int maxPlayers{0};
    bool passworded{false};
};

// Caps for the tolerant scanner (bounds memory against a hostile/huge response).
inline constexpr std::size_t kMaxLobbyServers = 1024;
inline constexpr std::size_t kMaxLobbyStringBytes = 256;

// Parse a lobby server-list JSON document into LobbyServer rows. Deliberately a tolerant, bounded
// hand-rolled scanner (no JSON library in-tree; the ServerTickReport idiom): it accepts a top-level
// array of flat objects with string keys `name` / `host` / `port` / `mode` / `mission` / `players` /
// `max_players` / `passworded`, ignores unknown keys, never reads out of bounds, and caps both the row
// count and per-string length. A malformed document yields the rows parsed so far (possibly empty). A
// row with no host or a zero port is dropped.
[[nodiscard]] std::vector<LobbyServer> parseLobbyServerList(std::string_view json);

// Fetches a lobby's server list over HTTP (GET <lobbyUrl>/v1/servers) via an injected IHttpClient, and
// parses the response into LobbyServer rows. Main-thread only; drive the client's service() each frame.
// One in-flight request at a time per lobby URL. Never touches the sim thread.
class LobbyListClient : public IHttpClientHandler {
  public:
    LobbyListClient(IHttpClient& http, ILogger& log);

    // Start a fetch of <lobbyUrl>/v1/servers. A fetch already in flight is a no-op. Returns false when
    // the request could not be enqueued (no HTTP backend / empty url).
    bool refresh(const std::string& lobbyUrl);

    [[nodiscard]] bool inFlight() const noexcept {
        return m_reqId != 0;
    }
    // The rows from the most recent successful fetch (empty until one completes).
    [[nodiscard]] const std::vector<LobbyServer>& servers() const noexcept {
        return m_servers;
    }
    [[nodiscard]] bool lastFetchFailed() const noexcept {
        return m_lastFailed;
    }

    // IHttpClientHandler.
    bool onHttpData(HttpRequestId id, const void* data, std::size_t len) override;
    void onHttpComplete(HttpRequestId id, HttpStatus status, long httpCode, const char* errorMsg) override;

  private:
    IHttpClient& m_http;
    ILogger& m_log;
    HttpRequestId m_reqId{0};
    std::string m_buf; // accumulating response body
    std::vector<LobbyServer> m_servers;
    bool m_lastFailed{false};
};

} // namespace fl
