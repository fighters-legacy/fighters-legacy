// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// Per-IP failed-auth tracker. Counts consecutive failures per source IP and
// enforces a TTL lockout after a configurable threshold is reached. Single-
// threaded — callers are responsible for serialization.
//
// Used by WorldBroadcaster (MsgAdminCommand operator channel) and RconServer
// (Source Engine RCON TCP channel).
class AuthTracker {
  public:
    struct FailureEntry {
        std::string ip;
        bool lockedOut;
        int failures;        // 0 when lockedOut == true (counter erased on lockout)
        long long expiresIn; // seconds remaining; 0 when lockedOut == false
    };

    AuthTracker(int maxFailures, int lockoutSeconds)
        : m_maxFailures(maxFailures), m_lockoutDuration(lockoutSeconds),
          m_now([] { return std::chrono::steady_clock::now(); }) {}

    // Record a failed auth attempt. Returns true if the IP is now locked out.
    bool recordFailure(const std::string& ip) {
        auto& count = m_failCount[ip];
        ++count;
        if (count >= m_maxFailures) {
            m_lockouts[ip] = m_now() + m_lockoutDuration;
            m_failCount.erase(ip);
            return true;
        }
        return false;
    }

    // Clear the failure counter on successful auth. Does not clear an active lockout.
    void recordSuccess(const std::string& ip) {
        m_failCount.erase(ip);
    }

    // Returns true if the IP is locked out and the lockout has not yet expired.
    // Lazily clears expired entries on each call.
    bool isLockedOut(const std::string& ip) {
        auto it = m_lockouts.find(ip);
        if (it == m_lockouts.end())
            return false;
        if (m_now() >= it->second) {
            m_lockouts.erase(it);
            return false;
        }
        return true;
    }

    // Remove all expired lockout entries. Call periodically to bound memory growth.
    void pruneExpired() {
        auto now = m_now();
        for (auto it = m_lockouts.begin(); it != m_lockouts.end();) {
            if (now >= it->second)
                it = m_lockouts.erase(it);
            else
                ++it;
        }
    }

    // Clear the active lockout and failure counter for an IP immediately.
    // No-op if the IP is not currently locked out (idempotent).
    void clearLockout(const std::string& ip) {
        m_lockouts.erase(ip);
        m_failCount.erase(ip);
    }

    // Inject a deterministic clock for unit tests (mirrors setClockOverride pattern).
    void setClockOverride(std::function<std::chrono::steady_clock::time_point()> fn) {
        m_now = std::move(fn);
    }

    int maxFailures() const noexcept {
        return m_maxFailures;
    }

    // Count of non-expired lockouts. Does not prune.
    int lockedOutCount() const {
        auto now = m_now();
        int count = 0;
        for (const auto& [ip, expiry] : m_lockouts)
            if (now < expiry)
                ++count;
        return count;
    }

    // Snapshot of all active lockouts + IPs with pending failures.
    // Expired lockouts are excluded inline (no pruning).
    std::vector<FailureEntry> failureSummary() const {
        auto now = m_now();
        std::vector<FailureEntry> result;
        for (const auto& [ip, expiry] : m_lockouts)
            if (now < expiry) {
                long long secs = std::chrono::duration_cast<std::chrono::seconds>(expiry - now).count();
                result.push_back({ip, true, 0, secs});
            }
        for (const auto& [ip, count] : m_failCount)
            result.push_back({ip, false, count, 0LL});
        return result;
    }

  private:
    int m_maxFailures;
    std::chrono::seconds m_lockoutDuration;
    std::unordered_map<std::string, int> m_failCount;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lockouts;
    std::function<std::chrono::steady_clock::time_point()> m_now;
};

// Snapshot of auth lockout state. Returned by WorldBroadcaster::getAuthLockoutSummary()
// and RconServer::getRconAuthSummary().
struct AuthLockoutSummary {
    int activeCount{0};                             // non-expired lockout count
    int threshold{0};                               // configured maxFailures value
    std::vector<AuthTracker::FailureEntry> entries; // active lockouts + IPs with pending failures
};

} // namespace fl
