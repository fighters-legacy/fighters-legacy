// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/ServerBrowserModel.h"

#include "net/GameProtocol.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace fl {

namespace {
std::string keyOf(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}
// A fixed wire char[] treated as a C string, bounded by its capacity.
std::string cstr(const char* p, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && p[n] != '\0')
        ++n;
    return std::string(p, n);
}
} // namespace

void ServerBrowserModel::rebuild(const std::vector<DiscoveryListener::ServerInfo>& lan,
                                 const std::vector<LobbyServer>& lobby,
                                 const std::vector<ServerQueryClient::Result>& queries) {
    m_rows.clear();
    std::unordered_map<std::string, std::size_t> index; // host:port -> row index (dedup, LAN wins)

    // LAN rows first (authoritative + low-latency).
    for (const DiscoveryListener::ServerInfo& s : lan) {
        BrowserRow r;
        r.name = cstr(s.beacon.name, sizeof(s.beacon.name));
        r.host = s.address;
        r.gamePort = s.beacon.gamePort;
        r.queryPort = s.beacon.queryPort;
        r.players = s.beacon.playerCount;
        r.maxPlayers = s.beacon.maxPlayers;
        r.passworded = (s.beacon.gameModeFlags & kGameModePassworded) != 0u;
        r.shuttingDown = s.shuttingDown();
        r.shutdownSeconds = s.shutdownSeconds();
        r.source = BrowserSource::Lan;
        r.protocolMismatch = s.beacon.protocolVersion != kProtocolVersion;
        index[keyOf(r.host, r.gamePort)] = m_rows.size();
        m_rows.push_back(std::move(r));
    }

    // Lobby rows — skip any already known from LAN (dedup by host:port).
    for (const LobbyServer& s : lobby) {
        const std::string k = keyOf(s.host, s.port);
        if (index.count(k) != 0u)
            continue;
        BrowserRow r;
        r.name = s.name;
        r.host = s.host;
        r.gamePort = s.port;
        r.mode = s.mode;
        r.mission = s.mission;
        r.players = s.players;
        r.maxPlayers = s.maxPlayers;
        r.passworded = s.passworded;
        r.source = BrowserSource::Lobby;
        index[k] = m_rows.size();
        m_rows.push_back(std::move(r));
    }

    // Attach live server-info query results (ping + fresh counts) to matching rows.
    for (const ServerQueryClient::Result& q : queries) {
        const std::string k = keyOf(q.address, q.info.gamePort);
        auto it = index.find(k);
        if (it == index.end())
            continue;
        BrowserRow& r = m_rows[it->second];
        r.hasPing = true;
        r.pingMs = q.rttMs;
        r.players = q.info.playerCount;
        r.maxPlayers = q.info.maxPlayers;
        r.passworded = (q.info.gameModeFlags & kGameModePassworded) != 0u;
        r.shuttingDown = (q.info.gameModeFlags & kGameModeShuttingDown) != 0u;
        r.shutdownSeconds = q.info.shutdownSeconds;
        r.protocolMismatch = q.info.protocolVersion != kProtocolVersion;
        if (const std::string nm = cstr(q.info.name, sizeof(q.info.name)); !nm.empty())
            r.name = nm;
        if (const std::string md = cstr(q.info.modeId, sizeof(q.info.modeId)); !md.empty())
            r.mode = md;
        if (const std::string ms = cstr(q.info.mission, sizeof(q.info.mission)); !ms.empty())
            r.mission = ms;
        if (r.queryPort == 0)
            r.queryPort = q.queryPort;
    }

    // Sort: joinable (not shutting down, compatible protocol) first, then by descending player count,
    // then name for a stable order.
    std::sort(m_rows.begin(), m_rows.end(), [](const BrowserRow& a, const BrowserRow& b) {
        const int aJoin = (!a.shuttingDown && !a.protocolMismatch) ? 1 : 0;
        const int bJoin = (!b.shuttingDown && !b.protocolMismatch) ? 1 : 0;
        if (aJoin != bJoin)
            return aJoin > bJoin;
        if (a.players != b.players)
            return a.players > b.players;
        return a.name < b.name;
    });
}

} // namespace fl
