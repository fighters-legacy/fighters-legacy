// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/AdminChannel.h"

#include <algorithm>
#include <utility>

namespace fl {

AdminChannel::AdminChannel(Dispatcher dispatcher, Config config, const IClock& clock)
    : m_dispatch(std::move(dispatcher)), m_config(std::move(config)), m_clock(&clock),
      m_auth(m_config.maxAuthFailures, m_config.lockoutSeconds) {
    m_auth.setClock(clock);
}

std::string AdminChannel::dispatch(std::string_view line, const CommandIssuer& issuer) const {
    if (!m_dispatch)
        return "admin channel '" + m_config.name + "' has no dispatcher";
    return m_dispatch(line, issuer);
}

bool AdminChannel::lockedOut(const std::string& ip) {
    return !ip.empty() && m_auth.isLockedOut(ip);
}

bool AdminChannel::recordAuthResult(const std::string& ip, bool authenticated, bool attempted) {
    if (ip.empty())
        return false;
    if (authenticated) {
        m_auth.recordSuccess(ip);
        return false;
    }
    // An absent credential is a permission refusal, not a brute-force attempt. Counting it would let a
    // peer with no grant lock out the operator who shares its NAT address (#947).
    if (!attempted)
        return false;
    return m_auth.recordFailure(ip);
}

bool AdminChannel::clearLockout(const std::string& ip) {
    const bool wasLocked = m_auth.isLockedOut(ip);
    m_auth.clearLockout(ip);
    return wasLocked;
}

void AdminChannel::pruneExpiredLockouts() {
    m_auth.pruneExpired();
}

AuthLockoutSummary AdminChannel::authSummary() const {
    AuthLockoutSummary s;
    s.threshold = m_auth.maxFailures();
    s.activeCount = m_auth.lockedOutCount();
    s.entries = m_auth.failureSummary();
    return s;
}

void AdminChannel::setShellTap(std::function<int()> markFn, std::function<std::vector<std::string>(int)> drainFn) {
    m_mark = std::move(markFn);
    m_drain = std::move(drainFn);
}

void AdminChannel::armDrain(uint64_t token) {
    if (!m_mark)
        return; // no shell tap: this channel has no drain, which is the honest state for HTTP and MCP
    cancelDrain(token);
    m_pending.push_back(Pending{token, m_mark(), m_clock->now() + m_config.ackDrainDelay});
}

void AdminChannel::cancelDrain(uint64_t token) {
    m_pending.erase(
        std::remove_if(m_pending.begin(), m_pending.end(), [token](const Pending& p) { return p.token == token; }),
        m_pending.end());
}

void AdminChannel::serviceDrains(
    const std::function<void(uint64_t token, const std::vector<std::string>& lines)>& emit) {
    if (m_pending.empty() || !m_drain)
        return;
    const auto now = m_clock->now();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now < it->deadline) {
            ++it;
            continue;
        }
        const std::vector<std::string> lines = m_drain(it->mark);
        if (!lines.empty())
            emit(it->token, lines);
        it = m_pending.erase(it);
    }
}

// ---------------------------------------------------------------------------
// AdminChannelRegistry
// ---------------------------------------------------------------------------

void AdminChannelRegistry::add(AdminChannel& channel) {
    m_channels.push_back(&channel);
}

std::vector<std::string> AdminChannelRegistry::clearLockoutEverywhere(const std::string& ip) {
    std::vector<std::string> cleared;
    for (AdminChannel* c : m_channels)
        if (c->clearLockout(ip))
            cleared.push_back(c->name());
    return cleared;
}

std::vector<std::pair<std::string, AuthLockoutSummary>> AdminChannelRegistry::summaries() const {
    std::vector<std::pair<std::string, AuthLockoutSummary>> out;
    out.reserve(m_channels.size());
    for (AdminChannel* c : m_channels)
        out.emplace_back(c->name(), c->authSummary());
    return out;
}

int AdminChannelRegistry::activeLockoutCount() const {
    int total = 0;
    for (AdminChannel* c : m_channels)
        total += c->authSummary().activeCount;
    return total;
}

} // namespace fl
