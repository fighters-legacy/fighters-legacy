// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/LobbyRegistration.h"

#include "ILogger.h"
#include "util/Json.h" // the engine's one JSON escaper (#1080)

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace fl {

LobbyRegistration::LobbyRegistration(IHttpClient& http, ILogger& log) : m_http(http), m_log(log) {}

void LobbyRegistration::configure(const LobbyRegistrationConfig& cfg) {
    m_cfg = cfg;
    m_cfg.heartbeatS = std::clamp(m_cfg.heartbeatS, 5, 300);
    m_players = 0;
    m_enabled = m_cfg.visibilityPublic && !m_cfg.lobbyUrl.empty();
    m_havePosted = false;
    m_backoffMultiplier = 1;
    m_warnedFailure = false;
}

void LobbyRegistration::setDynamic(int players, const std::string& mission) {
    m_players = players;
    m_cfg.mission = mission;
}

std::string LobbyRegistration::endpoint() const {
    std::string url = m_cfg.lobbyUrl;
    if (!url.empty() && url.back() == '/')
        url.pop_back();
    url += "/v1/servers";
    return url;
}

std::string LobbyRegistration::buildBody() const {
    std::string b = "{";
    b += "\"name\":" + json::str(m_cfg.name);
    b += ",\"port\":" + std::to_string(m_cfg.gamePort);
    b += ",\"players\":" + std::to_string(m_players);
    b += ",\"max_players\":" + std::to_string(m_cfg.maxPlayers);
    b += ",\"mode\":" + json::str(m_cfg.mode);
    b += ",\"mission\":" + json::str(m_cfg.mission);
    b += ",\"visibility\":\"public\"";
    b += "}";
    return b;
}

void LobbyRegistration::postHeartbeat() {
    if (m_reqId != 0)
        return; // one in flight
    HttpRequestOptions opts;
    opts.url = endpoint();
    opts.method = HttpMethod::Post;
    opts.contentType = "application/json";
    m_lastBody = buildBody();
    opts.body = m_lastBody;
    m_reqId = m_http.request(opts, this);
    m_lastPost = m_clock->now();
    m_havePosted = true;
}

void LobbyRegistration::tick() {
    if (!m_enabled || m_reqId != 0)
        return;
    const auto now = m_clock->now();
    const auto interval = std::chrono::seconds(static_cast<long long>(m_cfg.heartbeatS) * m_backoffMultiplier);
    if (!m_havePosted || now - m_lastPost >= interval)
        postHeartbeat();
}

void LobbyRegistration::deregister() {
    if (!m_enabled)
        return;
    HttpRequestOptions opts;
    opts.url = endpoint();
    opts.method = HttpMethod::Delete_;
    opts.contentType = "application/json";
    opts.body = "{\"port\":" + std::to_string(m_cfg.gamePort) + "}";
    (void)m_http.request(opts, this); // best-effort; the reply is ignored
}

void LobbyRegistration::onHttpComplete(HttpRequestId id, HttpStatus status, long httpCode, const char* errorMsg) {
    if (id != m_reqId)
        return;
    m_reqId = 0;
    if (status == HttpStatus::Success && httpCode >= 200 && httpCode < 300) {
        m_backoffMultiplier = 1; // recovered
        m_warnedFailure = false;
        return;
    }
    // Failure: warn once, back off (cap the multiplier so heartbeat*mult <= ~5 min).
    if (!m_warnedFailure) {
        m_warnedFailure = true;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "lobby registration POST failed (code %ld): %s; backing off", httpCode,
                      errorMsg ? errorMsg : "");
        m_log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
    }
    const int maxMult = std::max(1, 300 / std::max(1, m_cfg.heartbeatS));
    m_backoffMultiplier = std::min(maxMult, m_backoffMultiplier * 2);
}

} // namespace fl
