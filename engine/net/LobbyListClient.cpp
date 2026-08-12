// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/LobbyListClient.h"

#include "util/Json.h"

#include "ILogger.h"

#include <cstdio>
#include <cstdlib>

namespace fl {

namespace {

// The whole of this file's JSON reading used to be a bespoke scanner -- parseJsonString,
// readBareToken, skipValue and a hand-rolled key/value walk, the fifth independent reader in the tree
// (#1080). engine/util/Json.h carries the one reader now; what stays here is only what is specific to
// this payload: the caps, and the two spellings the lobby accepts for a couple of fields.

// The caps on this untrusted payload are declared in the header and unchanged: kMaxLobbyServers and
// kMaxLobbyStringBytes. A refused element is simply absent, not an error.

// First non-empty of the accepted spellings for one field. The lobby REST shape is not frozen, so
// `host`/`address` and `max_players`/`maxPlayers` are both read rather than one being declared wrong.
[[nodiscard]] std::string firstString(std::string_view obj, std::initializer_list<std::string_view> keys) {
    for (const std::string_view k : keys)
        if (auto v = json::stringField(obj, k, kMaxLobbyStringBytes); v && !v->empty())
            return *v;
    return {};
}

[[nodiscard]] long long firstInt(std::string_view obj, std::initializer_list<std::string_view> keys,
                                 long long fallback) {
    for (const std::string_view k : keys)
        if (auto v = json::intField(obj, k))
            return *v;
    return fallback;
}

} // namespace

std::vector<LobbyServer> parseLobbyServerList(std::string_view s) {
    std::vector<LobbyServer> out;
    for (const std::string_view obj : json::arrayElements(s, kMaxLobbyServers)) {
        if (!json::isObject(obj))
            continue; // an element that is not a server object is skipped, not fatal

        LobbyServer sv;
        sv.host = firstString(obj, {"host", "address"});
        sv.port = static_cast<uint16_t>(firstInt(obj, {"port"}, 0) & 0xFFFF);
        // A row without both an address and a port is not joinable, so it is not a row. Unchanged
        // rule; it just reads as one line now instead of a flag threaded through the scan.
        if (sv.host.empty() || sv.port == 0)
            continue;

        sv.name = firstString(obj, {"name"});
        sv.mode = firstString(obj, {"mode"});
        sv.mission = firstString(obj, {"mission"});
        sv.players = static_cast<int>(firstInt(obj, {"players"}, 0));
        sv.maxPlayers = static_cast<int>(firstInt(obj, {"max_players", "maxPlayers"}, 0));
        sv.passworded = json::boolField(obj, "passworded").value_or(false);
        out.push_back(std::move(sv));
    }
    return out;
}

LobbyListClient::LobbyListClient(IHttpClient& http, ILogger& log) : m_http(http), m_log(log) {}

bool LobbyListClient::refresh(const std::string& lobbyUrl) {
    if (m_reqId != 0 || lobbyUrl.empty())
        return false;
    HttpRequestOptions opts;
    opts.url = lobbyUrl;
    if (!opts.url.empty() && opts.url.back() == '/')
        opts.url.pop_back();
    opts.url += "/v1/servers";
    opts.method = HttpMethod::Get;
    m_buf.clear();
    m_lastFailed = false;
    m_reqId = m_http.get(opts, this);
    return m_reqId != 0;
}

bool LobbyListClient::onHttpData(HttpRequestId id, const void* data, std::size_t len) {
    if (id != m_reqId)
        return true;
    // Bound the accumulated body (a lobby list is small).
    if (m_buf.size() + len <= 1u * 1024u * 1024u)
        m_buf.append(static_cast<const char*>(data), len);
    return true;
}

void LobbyListClient::onHttpComplete(HttpRequestId id, HttpStatus status, long httpCode, const char* errorMsg) {
    if (id != m_reqId)
        return;
    m_reqId = 0;
    if (status != HttpStatus::Success || httpCode < 200 || httpCode >= 300) {
        m_lastFailed = true;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "lobby list fetch failed (code %ld): %s", httpCode, errorMsg ? errorMsg : "");
        m_log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
        return;
    }
    m_servers = parseLobbyServerList(m_buf);
    m_buf.clear();
}

} // namespace fl
