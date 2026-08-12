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
    if (!m_config.perIpAuth || ip.empty())
        return false;
    std::lock_guard<std::mutex> lk(m_authMutex);
    return m_auth.isLockedOut(ip);
}

bool AdminChannel::recordAuthResult(const std::string& ip, bool authenticated, bool attempted) {
    if (!m_config.perIpAuth || ip.empty())
        return false;
    std::lock_guard<std::mutex> lk(m_authMutex);
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
    if (!m_config.perIpAuth)
        return false;
    std::lock_guard<std::mutex> lk(m_authMutex);
    const bool wasLocked = m_auth.isLockedOut(ip);
    m_auth.clearLockout(ip);
    return wasLocked;
}

void AdminChannel::pruneExpiredLockouts() {
    if (!m_config.perIpAuth)
        return;
    std::lock_guard<std::mutex> lk(m_authMutex);
    m_auth.pruneExpired();
}

AuthLockoutSummary AdminChannel::authSummary() const {
    AuthLockoutSummary s;
    // A channel with no credential reports a zero threshold rather than the default 5: a threshold
    // nothing can ever reach reads as "protected" to the operator scanning this during an incident.
    if (!m_config.perIpAuth)
        return s;
    std::lock_guard<std::mutex> lk(m_authMutex);
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

void AdminChannel::cancelDrainsWhere(const std::function<bool(uint64_t token)>& pred) {
    m_pending.erase(
        std::remove_if(m_pending.begin(), m_pending.end(), [&pred](const Pending& p) { return pred(p.token); }),
        m_pending.end());
}

std::optional<std::chrono::steady_clock::time_point> AdminChannel::nextDrainDeadline() const {
    if (m_pending.empty())
        return std::nullopt;
    auto it = std::min_element(m_pending.begin(), m_pending.end(),
                               [](const Pending& a, const Pending& b) { return a.deadline < b.deadline; });
    return it->deadline;
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
