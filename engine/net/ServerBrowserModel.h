// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/DiscoveryListener.h" // ServerInfo
#include "net/LobbyListClient.h"   // LobbyServer
#include "net/ServerQueryClient.h" // ServerQueryClient::Result

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

enum class BrowserSource : uint8_t {
    Lan,
    Lobby,
};

// One row in the server browser (#143): the merged, deduplicated view of a server known from LAN
// discovery and/or a lobby, enriched with a live server-info query (ping / accurate player counts).
struct BrowserRow {
    std::string name;
    std::string host; // address/host the client connects to
    uint16_t gamePort{0};
    uint16_t queryPort{0}; // 0 = query unavailable
    std::string mode;
    std::string mission;
    int players{0};
    int maxPlayers{0};
    bool passworded{false};
    bool shuttingDown{false};
    uint16_t shutdownSeconds{0};
    BrowserSource source{BrowserSource::Lobby};
    bool hasPing{false};
    float pingMs{0.f};
    bool protocolMismatch{false}; // advertised/queried protocol != this client's kProtocolVersion
    // #1074: the server's BUILD, advertised in the beacon and the query reply. Empty = the server did
    // not advertise one (it predates the field), which is NOT a mismatch. buildMismatch is set only
    // when both sides advertised and they differ — protocolMismatch's sibling, and the one that
    // actually fires during primary development, because kProtocolVersion stays 1 for every additive
    // change and so protocol agreement says nothing about whether the two builds understand each other.
    std::string build;
    bool buildMismatch{false};
};

// Merges LAN discovery, lobby listings, and server-info query results into a sorted, deduplicated row
// list. Pure logic (no sockets, no time): the browser screen feeds it snapshots each refresh and reads
// rows(). Dedup is by host:gamePort with LAN winning over a lobby entry (a LAN row is authoritative and
// low-latency). A query result attaches ping + fresh counts to a matching row.
class ServerBrowserModel {
  public:
    void rebuild(const std::vector<DiscoveryListener::ServerInfo>& lan, const std::vector<LobbyServer>& lobby,
                 const std::vector<ServerQueryClient::Result>& queries);

    // This client's build, for the per-row build-mismatch flag (#1074). Empty (the default) means the
    // model never reports a mismatch — a browser that does not know its own build cannot judge.
    void setClientBuildVersion(std::string version) {
        m_clientBuild = std::move(version);
    }

    [[nodiscard]] const std::vector<BrowserRow>& rows() const noexcept {
        return m_rows;
    }

  private:
    std::vector<BrowserRow> m_rows;
    std::string m_clientBuild; // #1074: empty = do not judge builds
};

} // namespace fl
