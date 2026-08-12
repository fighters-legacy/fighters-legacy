// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/AuthTracker.h"
#include "net/Capability.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fl {

// AdminChannel (#1079, D14) — one admin frontend's auth, issuer, dispatch and drain.
//
// Six frontends -- stdin, RCON, the ENet MsgAdminCommand path, the mission `do:` sink, HTTP REST and
// MCP -- route into the same CommandRegistry over the same ~43 commands with the same CapabilityMask
// gate. That substrate is the best thing in the server, and McpEndpoint states the invariant outright:
// "this class never decides what a command is permitted to do... that refusal is the proof MCP is a
// frontend rather than a parallel admin path."
//
// Everything AROUND dispatch was forked:
//
//   * THREE AuthTracker instances with no registry, surfaced through three separate context hooks, so
//     `admin_auth_status` and `admin_unlock` named each channel by hand -- and admin_unlock's own
//     comment said that unlocking two of three and leaving the third "would be worse than not
//     unlocking at all". A fourth channel would have been silently missed. ServerCommands.h named the
//     risk: "a third authentication surface an operator cannot see or clear is a surface that gets
//     forgotten during an incident."
//   * TWO hand-rolled async-ack drains, in WorldBroadcaster and RconServer, which independently picked
//     the same 20 ms deadline and the same mark-after-dispatch trick. Three frontends had none -- HTTP
//     holds a CommandShell* and never reads it, which its own header admits.
//   * THREE frontends used the capability-BYPASSING dispatch(line) overload (stdin, RCON, the mission
//     sink), so capability enforcement was a property of which frontend you arrived on rather than of
//     the command. That overload is deleted; every dispatch now carries an issuer.
//
// What is deliberately NOT here: the credential LADDER. Checking an operator password against a
// constant-time compare, a bearer token against the `[http_admin]` table, or an RCON password is
// genuinely per-transport, and pretending otherwise would produce a class with a mode switch per
// frontend. What is common is the bookkeeping around the outcome -- the per-IP failure counting, the
// lockout, being enumerable to an operator during an incident -- and that is exactly what this owns.
// A channel reports its credential verdict through recordAuthResult(); the ladder stays with the
// transport that knows how to read a credential.
//
// A seventh frontend costs a constructor call and a registry line, rather than ~35 lines of forked auth
// plus a third drain implementation.
//
// Lives in engine-net, beside AuthTracker and Capability, and takes its dispatcher as a function rather
// than holding a CommandRegistry&. That is deliberate: engine-net does not link engine-console, which is
// why setAdminDispatch was a std::function in the first place, and consolidating auth is not a reason to
// invert a layer boundary. The invariant that every dispatch carries an issuer is enforced where it can
// be -- CommandRegistry::dispatch(line) is DELETED -- rather than by this signature alone.
//
// THREAD OWNERSHIP, because the three live channels do not share one:
//
//   * AUTH is cross-thread and is mutex-guarded here. A transport thread checks a credential while the
//     sim thread runs `admin_unlock` or `admin_auth_status` over the registry -- which is exactly why
//     RconServer and HttpAdminServer each carried their own mutex around their own AuthTracker. That
//     mutex now lives once, next to the state it protects, instead of once per frontend.
//   * DISPATCH and the DRAIN belong to the frontend that owns the channel: it arms and services on the
//     same thread (the sim thread for ENet, the I/O thread for RCON). They are deliberately not
//     locked -- a lock there would be a lie about a queue only one thread ever touches.
class AdminChannel {
  public:
    // Bound to CommandRegistry::dispatch(line, issuer) by whoever owns the registry.
    using Dispatcher = std::function<std::string(std::string_view line, const CommandIssuer& issuer)>;

    struct Config {
        // Shown to an operator by admin_auth_status, and the name admin_unlock reports. Stable and
        // lowercase: it is operator-facing text during an incident, not a debug label.
        std::string name;
        int maxAuthFailures{5};
        int lockoutSeconds{300};
        // False for a frontend that presents no per-IP credential -- stdin and the mission `do:` sink
        // are trusted local surfaces whose issuer is fl::systemIssuer(). Such a channel reports a zero
        // threshold and never accumulates entries, so an operator reading admin_auth_status sees "this
        // surface exists and has no lockout" rather than a threshold that nothing can ever reach. It is
        // still registered: the failure mode this issue fixes is a surface an operator cannot SEE.
        bool perIpAuth{true};
        // How long after a dispatch the channel waits before delivering shell output the command
        // produced asynchronously (via GameLoop::enqueueSimCallback). Wall-clock, not ticks, so drain
        // timing is immune to sim rate -- the property both hand-rolled drains had and neither stated.
        std::chrono::milliseconds ackDrainDelay{20};
    };

    AdminChannel(Dispatcher dispatcher, Config config, const IClock& clock);

    [[nodiscard]] const std::string& name() const noexcept {
        return m_config.name;
    }

    // False = this frontend has no per-IP credential to fail; see Config::perIpAuth.
    [[nodiscard]] bool hasPerIpAuth() const noexcept {
        return m_config.perIpAuth;
    }

    // ---- dispatch ------------------------------------------------------------------------------
    // Always capability-checked, because the dispatcher is the issuer-aware overload and there is no
    // other. There is no issuer-less form here, on purpose: the deleted CommandRegistry::dispatch(line)
    // is what let three frontends bypass the gate. A channel with no dispatcher returns a refusal
    // string rather than an empty one, so a mis-wired frontend is visible instead of silent.
    [[nodiscard]] std::string dispatch(std::string_view line, const CommandIssuer& issuer) const;

    // ---- auth ----------------------------------------------------------------------------------
    [[nodiscard]] bool lockedOut(const std::string& ip);

    // Record a credential check's outcome. `attempted` distinguishes a genuine attempt from a caller
    // that presented nothing -- an absent credential is a permission refusal, not a brute-force
    // attempt, and counting it would pollute the lockout (#947). Returns true when this failure just
    // triggered a lockout, which is the signal a transport uses to hang up.
    bool recordAuthResult(const std::string& ip, bool authenticated, bool attempted = true);

    // True when the IP had an active lockout that this call cleared.
    bool clearLockout(const std::string& ip);
    void pruneExpiredLockouts();

    [[nodiscard]] AuthLockoutSummary authSummary() const;

    // ---- async-ack drain -----------------------------------------------------------------------
    //
    // A mutating command runs through GameLoop::enqueueSimCallback, so its output reaches the shell
    // AFTER dispatch returns. The pattern both hand-rolled drains used: remember the shell's line mark
    // at dispatch time, set a deadline, and deliver whatever appeared after it. `token` is whatever the
    // transport needs to address the reply (an RCON packet id, an ENet reqId).
    //
    // Requires a shell; without one, arm() is a no-op and the channel simply has no drain -- which is
    // the honest state of the three frontends that had none.
    // markFn returns the shell's current line count; drainFn(mark) returns the lines written since it.
    // Both null = this channel has no drain, which is the honest state of the three frontends that had
    // none. Functions rather than a CommandShell* for the layering reason above.
    void setShellTap(std::function<int()> markFn, std::function<std::vector<std::string>(int)> drainFn);

    void armDrain(uint64_t token);
    void cancelDrain(uint64_t token);

    // Drop every armed drain whose token matches. The transports address a reply to something that can
    // go away before the deadline -- an ENet peer disconnects, an RCON client closes its socket -- and
    // both hand-rolled drains had their own erase-if for exactly that. Tokens are packed by the
    // transport, so the predicate is the transport's too.
    void cancelDrainsWhere(const std::function<bool(uint64_t token)>& pred);

    // Deliver every armed drain whose deadline has passed. `emit(token, lines)` is the transport.
    void serviceDrains(const std::function<void(uint64_t token, const std::vector<std::string>& lines)>& emit);

    // The soonest armed deadline, or nullopt when nothing is armed. RCON's poll loop clamps its
    // timeout to this so a drain fires on time instead of on the next 100 ms poll wakeup.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> nextDrainDeadline() const;

    [[nodiscard]] std::size_t pendingDrainCount() const noexcept {
        return m_pending.size();
    }

  private:
    struct Pending {
        uint64_t token{0};
        int mark{0};
        std::chrono::steady_clock::time_point deadline{};
    };

    Dispatcher m_dispatch;
    Config m_config;
    const IClock* m_clock;

    mutable std::mutex m_authMutex; // guards m_auth only; see THREAD OWNERSHIP above
    AuthTracker m_auth;

    std::function<int()> m_mark;
    std::function<std::vector<std::string>(int)> m_drain;
    std::vector<Pending> m_pending;
};

// Every AdminChannel in the process, so an operator can see and clear all of them at once.
//
// This is the registry ServerCommands.h asked for by warning about its absence. admin_auth_status and
// admin_unlock walk it instead of naming channels, so a new channel cannot be invisible during an
// incident -- which was the actual failure mode, not the duplicated code.
//
// The channel LIST is written only during init, before any frontend starts, and read-only afterwards;
// the per-channel state the reads reach is mutex-guarded by AdminChannel. So walking the registry from
// the sim thread while a transport thread authenticates is safe, which is the whole point of it.
class AdminChannelRegistry {
  public:
    // The channel must outlive the registry. Registered at construction time in fl-server's init.
    void add(AdminChannel& channel);

    [[nodiscard]] std::size_t size() const noexcept {
        return m_channels.size();
    }
    [[nodiscard]] const std::vector<AdminChannel*>& channels() const noexcept {
        return m_channels;
    }

    // Clear `ip` on every channel. Returns the names of the channels that had an active lockout, so
    // the operator is told what actually changed rather than a fixed list of channel names.
    std::vector<std::string> clearLockoutEverywhere(const std::string& ip);

    // (channel name, summary) for every channel, in registration order.
    [[nodiscard]] std::vector<std::pair<std::string, AuthLockoutSummary>> summaries() const;

    // Total active lockouts across all channels -- the number `status` reports.
    [[nodiscard]] int activeLockoutCount() const;

  private:
    std::vector<AdminChannel*> m_channels;
};

} // namespace fl
