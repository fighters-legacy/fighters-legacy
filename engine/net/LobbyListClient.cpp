// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/LobbyListClient.h"

#include "ILogger.h"

#include <cstdio>
#include <cstdlib>

namespace fl {

namespace {

// Skip ASCII whitespace.
void skipWs(std::string_view s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
}

// Parse a JSON string starting at s[i]=='"'. Advances i past the closing quote. Appends the decoded
// (bounded) content to `out`. Returns false on an unterminated string.
bool parseJsonString(std::string_view s, std::size_t& i, std::string& out) {
    out.clear();
    if (i >= s.size() || s[i] != '"')
        return false;
    ++i; // opening quote
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"')
            return true;
        if (c == '\\') {
            if (i >= s.size())
                return false;
            const char e = s[i++];
            char decoded = e;
            switch (e) {
            case 'n':
                decoded = '\n';
                break;
            case 't':
                decoded = '\t';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'u':
                // \uXXXX: keep it simple + bounded — emit '?' and skip 4 hex digits.
                for (int k = 0; k < 4 && i < s.size(); ++k)
                    ++i;
                decoded = '?';
                break;
            default:
                decoded = e; // \" \\ \/ and anything else: literal
                break;
            }
            if (out.size() < kMaxLobbyStringBytes)
                out.push_back(decoded);
        } else {
            if (out.size() < kMaxLobbyStringBytes)
                out.push_back(c);
        }
    }
    return false; // unterminated
}

// Read a bare (non-string) JSON token [number/true/false/null] as text, advancing i past it.
void readBareToken(std::string_view s, std::size_t& i, std::string& out) {
    out.clear();
    while (i < s.size()) {
        const char c = s[i];
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            break;
        if (out.size() < kMaxLobbyStringBytes)
            out.push_back(c);
        ++i;
    }
}

// Skip a JSON value we do not consume into a field (nested object/array/string/bare token), so the
// key/value scan stays aligned. Depth-bounded so a pathological input cannot recurse without limit.
void skipValue(std::string_view s, std::size_t& i, int depth) {
    skipWs(s, i);
    if (i >= s.size())
        return;
    const char c = s[i];
    if (c == '"') {
        std::string tmp;
        parseJsonString(s, i, tmp);
    } else if ((c == '{' || c == '[') && depth < 32) {
        const char close = (c == '{') ? '}' : ']';
        ++i;
        while (i < s.size() && s[i] != close) {
            skipWs(s, i);
            if (i < s.size() && s[i] == '"') {
                std::string tmp;
                parseJsonString(s, i, tmp);
            } else if (i < s.size() && (s[i] == '{' || s[i] == '[')) {
                skipValue(s, i, depth + 1);
            } else if (i < s.size() && s[i] != close) {
                ++i;
            }
        }
        if (i < s.size())
            ++i; // consume the close
    } else {
        std::string tmp;
        readBareToken(s, i, tmp);
    }
}

} // namespace

std::vector<LobbyServer> parseLobbyServerList(std::string_view s) {
    std::vector<LobbyServer> out;
    std::size_t i = 0;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '[')
        return out; // not an array
    ++i;

    while (i < s.size() && out.size() < kMaxLobbyServers) {
        skipWs(s, i);
        if (i >= s.size() || s[i] == ']')
            break;
        if (s[i] != '{') {
            // Not an object where one is expected — bail with what we have.
            break;
        }
        ++i; // enter object
        LobbyServer sv;
        bool haveHost = false;
        while (i < s.size() && s[i] != '}') {
            skipWs(s, i);
            if (i >= s.size() || s[i] == '}')
                break;
            if (s[i] != '"') {
                ++i; // stray separator (',' etc.)
                continue;
            }
            std::string key;
            if (!parseJsonString(s, i, key))
                break;
            skipWs(s, i);
            if (i < s.size() && s[i] == ':')
                ++i;
            skipWs(s, i);
            if (i >= s.size())
                break;

            if (s[i] == '"') {
                std::string val;
                parseJsonString(s, i, val);
                if (key == "name")
                    sv.name = val;
                else if (key == "host" || key == "address") {
                    sv.host = val;
                    haveHost = !val.empty();
                } else if (key == "mode")
                    sv.mode = val;
                else if (key == "mission")
                    sv.mission = val;
            } else if (s[i] == '{' || s[i] == '[') {
                skipValue(s, i, 0); // ignore nested structures
            } else {
                std::string tok;
                readBareToken(s, i, tok);
                if (key == "port")
                    sv.port = static_cast<uint16_t>(std::strtoul(tok.c_str(), nullptr, 10) & 0xFFFFu);
                else if (key == "players")
                    sv.players = static_cast<int>(std::strtol(tok.c_str(), nullptr, 10));
                else if (key == "max_players" || key == "maxPlayers")
                    sv.maxPlayers = static_cast<int>(std::strtol(tok.c_str(), nullptr, 10));
                else if (key == "passworded")
                    sv.passworded = (tok == "true" || tok == "1");
            }
            skipWs(s, i);
            if (i < s.size() && s[i] == ',')
                ++i;
        }
        if (i < s.size() && s[i] == '}')
            ++i; // leave object
        if (haveHost && sv.port != 0)
            out.push_back(std::move(sv));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',')
            ++i;
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
    m_reqId = m_http.get(opts);
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
