// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCommands.h"

#include "ConfigReload.h"

#include "AiControllerBuild.h" // the one AI-controller construction ladder (#1236)
#include "ai/AiControllerFactory.h"
#include "ai/WingmanCommand.h" // the six-command grammar (was transitive via the factory)
#include "atc/AtcService.h"    // atc_status/atc_scramble/atc_hold (#705)
#include "entity/EntityTypeRegistry.h"
#include "script/LuaController.h"
#include "server_config.h"
#include <ILogger.h>
#include <console/CommandRegistry.h>
#include <console/CommandShell.h>
#include <entity/EntityManager.h>
#include <entity/EntityState.h>
#include <loop/GameLoop.h>
#include <loop/TimeRate.h>
#include <net/DiscoveryBeacon.h>
#include <net/NetworkUtils.h> // normalizeIp + extractIp — the SAME pair admission matches on (#1243)
#include <net/WorldBroadcaster.h>
#include <net/WorldStateJson.h> // worldstate/events JSON (#600)
#include <util/Parse.h>         // the one strict number parse family (#1244)
#include <weather/WeatherController.h>
#include <weather/WeatherTypes.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace fl {

// ---------------------------------------------------------------------------
// Sim-callback output + parsing helpers (#610)
// ---------------------------------------------------------------------------

// Emit a line from INSIDE an enqueueSimCallback. Commands that mutate the world run on the sim
// thread, long after dispatch() returned its ack, so their real result has to reach the operator
// through stdout and (when RCON is configured) the shell ring that RconServer drains. Every existing
// mutating command open-codes exactly this; new ones should not add another copy.
static void printAdmin(const ServerCommandContext& ctx, std::string_view line) {
    std::printf("%.*s\n", static_cast<int>(line.size()), line.data());
    if (ctx.rcon.shell)
        ctx.rcon.shell->print(std::string(line));
    std::fflush(stdout);
}

// Call-shape adapter over util/Parse.h (#1244); the rule lives there.
static bool parseU32(std::string_view s, uint32_t& out) {
    return fl::readInto(fl::parseU32(s), out);
}

// Parse a duration string into seconds.
// Accepts: bare integer (seconds), Ns, Nm, Nh, and compound NhNm.
// Returns nullopt on parse error.
static std::optional<uint32_t> parseDurationSecs(std::string_view s) {
    if (s.empty())
        return std::nullopt;

    uint32_t total = 0;
    bool consumed = false;

    auto parseNum = [&](std::string_view& v, uint32_t& out) -> bool {
        if (v.empty() || v[0] < '0' || v[0] > '9')
            return false;
        uint64_t n = 0;
        std::size_t i = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            n = n * 10 + static_cast<uint64_t>(v[i] - '0');
            ++i;
        }
        if (n > UINT32_MAX)
            return false;
        out = static_cast<uint32_t>(n);
        v.remove_prefix(i);
        return true;
    };

    uint32_t n = 0;
    while (!s.empty()) {
        if (!parseNum(s, n))
            return std::nullopt;
        consumed = true;
        if (s.empty()) {
            total += n; // bare integer → seconds
            break;
        }
        char unit = s[0];
        s.remove_prefix(1);
        if (unit == 's' || unit == 'S') {
            total += n;
        } else if (unit == 'm' || unit == 'M') {
            total += n * 60u;
        } else if (unit == 'h' || unit == 'H') {
            total += n * 3600u;
        } else {
            return std::nullopt;
        }
    }

    return consumed ? std::optional<uint32_t>(total) : std::nullopt;
}

static std::string formatSecs(long long secs) {
    if (secs <= 0)
        return "0s";
    long long m = secs / 60, s = secs % 60;
    char buf[32];
    if (m > 0)
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", m, s);
    else
        std::snprintf(buf, sizeof(buf), "%llds", secs);
    return buf;
}

static std::string formatAuthSection(const std::string& label, const fl::AuthLockoutSummary& s, bool perIpAuth = true) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[admin] %s:", label.c_str());
    std::string out(buf);
    // A trusted local surface (stdin, the mission `do:` sink) has no credential to fail. Say so, rather
    // than printing an empty section an operator has to interpret as either "clean" or "not wired".
    if (!perIpAuth) {
        out += "\n[admin]   no per-IP authentication (trusted local surface)";
        return out;
    }
    for (const auto& e : s.entries) {
        out += '\n';
        if (e.lockedOut)
            std::snprintf(buf, sizeof(buf), "[admin]   %-37s locked out -- expires in %s", e.ip.c_str(),
                          formatSecs(e.expiresIn).c_str());
        else
            std::snprintf(buf, sizeof(buf), "[admin]   %-37s %d failure(s) (threshold: %d)", e.ip.c_str(), e.failures,
                          s.threshold);
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------
// registerServerCommands
// ---------------------------------------------------------------------------

void registerServerCommands(CommandRegistry& registry, std::shared_ptr<const ServerCommandContext> ctx) {

    // help [command]
    registry.registerCommand("help", "help [command]  -- list all commands or show usage for one", 0,
                             [&registry](std::span<std::string_view> args) -> std::string {
                                 if (!args.empty())
                                     return registry.helpFor(args[0]);
                                 return registry.helpText();
                             });

    // status
    registry.registerCommand(
        "status", "status  -- show server state (uptime, peer count, entity count, tick rate)", 0,
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx->sim.broadcaster || !ctx->sim.entityManager)
                return "status: not available";
            // The SERVER's uptime, from the one instance every frontend shares -- `/health` reports
            // the same number by construction rather than by two implementations agreeing (#1048).
            const long long uptimeSec = ctx->env.uptime.seconds();
            int peers = ctx->sim.broadcaster->getPeerCount();
            uint32_t entities = ctx->sim.entityManager->liveCount();
            const fl::TickBudget tb = ctx->sim.broadcaster->getTickBudget();
            // Entity count reads as "N" uncapped and "N/cap" when world.entity_soft_cap is set (#1049):
            // a cap that is doing something must be visible in the one command an operator always runs.
            const uint32_t cap = ctx->sim.entityManager->softCap();
            char entBuf[48];
            if (cap > 0)
                std::snprintf(entBuf, sizeof(entBuf), "%u/%u", entities, cap);
            else
                std::snprintf(entBuf, sizeof(entBuf), "%u", entities);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "uptime: %llds  peers: %d  entities: %s  tick: %.1f Hz (%.2f/%.2f ms mean/p99)", uptimeSec,
                          peers, entBuf, tb.tickHz, tb.total.mean, tb.total.p99);
            std::string out(buf);
            // Refusals are reported only once there are any — a permanent "refused: 0" trains the eye
            // to skip the field that matters.
            if (const uint64_t refusals = ctx->sim.entityManager->softCapRefusals(); refusals > 0) {
                char rbuf[80];
                std::snprintf(rbuf, sizeof(rbuf), "  cap refusals: %llu", static_cast<unsigned long long>(refusals));
                out += rbuf;
            }
            const fl::OverrunStatus ov = ctx->sim.broadcaster->getOverrunStatus();
            char ovbuf[96];
            std::snprintf(ovbuf, sizeof(ovbuf), "  load: %.0f%%  interest: %.0f%%%s", ov.loadFactor * 100.0,
                          ov.interestScale * 100.0, ov.degraded ? " [DEGRADED]" : "");
            out += ovbuf;
            // Across EVERY admin frontend, not just the ENet one (#1079). The old count read one of
            // three trackers, so an operator locked out of RCON saw a clean `status`.
            const int lockouts = ctx->adminChannels ? ctx->adminChannels->activeLockoutCount() : 0;
            if (lockouts > 0) {
                char lbuf[96];
                std::snprintf(lbuf, sizeof(lbuf),
                              "\nadmin auth lockouts: %d active (use admin_auth_status for details)", lockouts);
                out += lbuf;
            }
            return out;
        });

    // tickstats — per-phase server tick budget (integrate/ai/collision/serialize/total).
    registry.registerCommand(
        "worldstate",
        "worldstate  -- the ~1 Hz aggregated world state as JSON (entities, factions, peers, mission, weather)", 0,
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx->sim.broadcaster)
                return "worldstate: not available";
            // Read the PUBLISHED snapshot, not broadcaster->worldState(): this handler runs on the
            // RCON thread or the stdin thread, never the sim thread, and the published copy is the
            // whole point of #600's off-thread publication.
            const auto snap = ctx->sim.broadcaster->worldStatePublisher().get();
            if (!snap)
                return "worldstate: no snapshot yet (the first rebuild is one second in)";
            return fl::toJson(*snap);
        });

    registry.registerCommand(
        "events", "events [after_seq] [max]  -- match event stream as JSON; omit after_seq for the recent tail", 0,
        [ctx](std::span<std::string_view> args) -> std::string {
            if (!ctx->sim.broadcaster)
                return "events: not available";
            fl::MatchEventLog& log = ctx->sim.broadcaster->matchEventLog();

            // No cursor = "show me what just happened"; a cursor = "everything I have not seen",
            // which is the shape a polling agent needs to avoid re-reading the same kills forever.
            std::vector<fl::MatchEvent> evs;
            uint64_t after = 0;
            bool gap = false;
            if (args.empty()) {
                constexpr std::size_t kDefaultTail = 32;
                evs = log.tail(kDefaultTail);
            } else {
                after = static_cast<uint64_t>(std::strtoull(std::string(args[0]).c_str(), nullptr, 10));
                gap = log.hasGapBefore(after);
                evs = log.since(after);
            }
            if (args.size() >= 2) {
                const std::size_t maxN =
                    static_cast<std::size_t>(std::strtoull(std::string(args[1]).c_str(), nullptr, 10));
                if (maxN > 0 && evs.size() > maxN)
                    evs.resize(maxN);
            }
            return fl::matchEventsToJson(std::span<const fl::MatchEvent>(evs), log.nextSeq(), gap);
        });

    registry.registerCommand(
        "tickstats", "tickstats  -- per-phase sim tick budget (ms: mean/p95/p99/max) + actual tick Hz", 0,
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx->sim.broadcaster)
                return "tickstats: not available";
            const fl::TickBudget tb = ctx->sim.broadcaster->getTickBudget();
            if (tb.ticksSampled == 0)
                return "tickstats: no ticks sampled yet";
            std::string out;
            char hdr[160];
            std::snprintf(hdr, sizeof(hdr), "tick %.2f Hz  window %.1fs  samples %llu (total %llu)", tb.tickHz,
                          tb.windowSeconds, static_cast<unsigned long long>(tb.ticksSampled),
                          static_cast<unsigned long long>(tb.ticksTotal));
            out += hdr;
            const fl::OverrunStatus ov = ctx->sim.broadcaster->getOverrunStatus();
            char ovrow[160];
            std::snprintf(ovrow, sizeof(ovrow),
                          "\n  overrun: load %.2f  snapshot %.1f Hz  ai_stride %u  interest %.2f%s", ov.loadFactor,
                          60.0 / static_cast<double>(ov.snapshotIntervalTicks), ov.aiStride, ov.interestScale,
                          ov.degraded ? "  [DEGRADED]" : "");
            out += ovrow;
            auto appendRow = [&out](const char* label, const fl::Stats& s) {
                char row[160];
                std::snprintf(row, sizeof(row), "\n  %-12s mean %.3f  p95 %.3f  p99 %.3f  max %.3f ms", label, s.mean,
                              s.p95, s.p99, s.max);
                out += row;
            };
            appendRow("total", tb.total);
            for (int i = 0; i < fl::kTickPhaseCount; ++i)
                appendRow(fl::tickPhaseName(static_cast<fl::TickPhase>(i)), tb.phases[i]);
            appendRow("other", tb.other);
            return out;
        });

    // peers
    registry.registerCommand("peers",
                             "peers  -- list connected peers (peerId, address, entity, delay, EWMA delay, jitter, buf "
                             "fill/max, send rate, throttle lever, loss)",
                             0, [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                     return "peers: not available";
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx]() {
                                     int count = 0;
                                     ctx->sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                                         char m[416];
                                         const std::string_view roleName = fl::rolePresetName(pi.caps);
                                         char roleCol[32] = "";
                                         if (!roleName.empty())
                                             std::snprintf(roleCol, sizeof(roleCol), "  role=%.*s",
                                                           static_cast<int>(roleName.size()), roleName.data());
                                         // #576: WHICH lever is decimating this peer. "who is being
                                         // decimated and by which lever" is the operator's actual
                                         // question, and the two answers call for opposite responses
                                         // -- shed work off the server, or look at that link.
                                         char throttleCol[48] = "";
                                         if (pi.governorBinding)
                                             std::snprintf(throttleCol, sizeof(throttleCol), "  throttle=SERVER(1/%u)",
                                                           pi.effectiveIntervalTicks);
                                         else if (pi.congestionBinding)
                                             std::snprintf(throttleCol, sizeof(throttleCol), "  throttle=link(1/%u)",
                                                           pi.effectiveIntervalTicks);
                                         std::snprintf(
                                             m, sizeof(m),
                                             "[admin] peer %u  %s  entity=%u/%u  delay=%ut (~%ums)"
                                             "  ewma=%.1ft  jitter=%.1ft  buf=%u/%u  rate=%.0fHz  loss=%.1f%%%s%s",
                                             pi.peerId, pi.addr.c_str(), pi.eid.index, pi.eid.generation, pi.delayTicks,
                                             (pi.delayTicks * 1000u + 30u) / 60u, pi.ewmaDelayTicks, pi.ewmaJitterTicks,
                                             pi.queueDepth, pi.bufferMaxDepth, static_cast<double>(pi.sendRateHz),
                                             static_cast<double>(pi.packetLoss) * 100.0, roleCol, throttleCol);
                                         printAdmin(*ctx, m);
                                         ++count;
                                     });
                                     if (count == 0)
                                         printAdmin(*ctx, "[admin] peers: no connected peers");
                                 });
                                 int count = ctx->sim.broadcaster->getPeerCount();
                                 char peerBuf[64];
                                 std::snprintf(peerBuf, sizeof(peerBuf), "%d peer(s) connected", count);
                                 return std::string(peerBuf);
                             });

    // kick <peerId|IP>
    registry.registerCommand("kick", "kick <peerId|IP>  -- disconnect a peer by ID or all peers from an IP address",
                             capBit(Capability::KickBan), [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: kick <peerId|IP>";
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                     return "kick: not available";
                                 std::string arg(args[0]);
                                 if (fl::isAllDigits(arg)) {
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return "kick: invalid peer ID";
                                     ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId]() {
                                         ctx->sim.broadcaster->kickPeer(peerId);
                                         char m[64];
                                         std::snprintf(m, sizeof(m), "[admin] kicked peer %u", peerId);
                                         printAdmin(*ctx, m);
                                     });
                                     char kickBuf[64];
                                     std::snprintf(kickBuf, sizeof(kickBuf), "kick: queued peer %u", peerId);
                                     return std::string(kickBuf);
                                 } else {
                                     std::string ip = normalizeIp(arg);
                                     ctx->sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                                         int kicked = 0;
                                         ctx->sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                                             if (extractIp(pi.addr) == ip) {
                                                 ctx->sim.broadcaster->kickPeer(pi.peerId);
                                                 ++kicked;
                                             }
                                         });
                                         char m[128];
                                         std::snprintf(m, sizeof(m), "[admin] kicked %d peer(s) from IP %s", kicked,
                                                       ip.c_str());
                                         printAdmin(*ctx, m);
                                     });
                                     return "kick: queued peers from IP " + ip;
                                 }
                             });

    // mute / unmute <peerId>  -- toggle a peer's chat mute (#646). Session-scoped, dropped on disconnect.
    for (const bool muteVal : {true, false}) {
        const char* name = muteVal ? "mute" : "unmute";
        const char* help = muteVal ? "mute <peerId>  -- silence a peer's chat for this session"
                                   : "unmute <peerId>  -- restore a muted peer's chat";
        registry.registerCommand(name, help, capBit(Capability::Mute),
                                 [ctx, muteVal, name](std::span<std::string_view> args) -> std::string {
                                     if (args.empty())
                                         return std::string("usage: ") + name + " <peerId>";
                                     if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                         return std::string(name) + ": not available";
                                     std::string arg(args[0]);
                                     if (!fl::isAllDigits(arg))
                                         return std::string(name) + ": expected a peer ID";
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return std::string(name) + ": invalid peer ID";
                                     ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId, muteVal, name]() {
                                         const bool ok = ctx->sim.broadcaster->setPeerMuted(peerId, muteVal);
                                         char m[80];
                                         std::snprintf(m, sizeof(m), "[admin] %s peer %u%s", name, peerId,
                                                       ok ? "" : " (unknown peer)");
                                         printAdmin(*ctx, m);
                                     });
                                     char buf[64];
                                     std::snprintf(buf, sizeof(buf), "%s: queued peer %u", name, peerId);
                                     return std::string(buf);
                                 });
    }

    // mutes  -- list currently muted peers (#646)
    registry.registerCommand("mutes", "mutes  -- list currently muted peers", 0,
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                     return "mutes: not available";
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx]() {
                                     const std::vector<uint32_t> ids = ctx->sim.broadcaster->mutedPeers();
                                     std::string line = "[admin] muted peers: ";
                                     if (ids.empty())
                                         line += "(none)";
                                     else
                                         for (std::size_t i = 0; i < ids.size(); ++i)
                                             line += (i ? ", " : "") + std::to_string(ids[i]);
                                     printAdmin(*ctx, line);
                                 });
                                 return "mutes: queued";
                             });

    // Voice comms (Epic J, #532). voice_mute gates TRANSMIT only — a muted peer still hears the net,
    // because muting someone is a moderation action against what they broadcast, not a punishment
    // that also blinds them to their own team.
    for (bool muteVal : {true, false}) {
        const char* name = muteVal ? "voice_mute" : "voice_unmute";
        const char* help = muteVal ? "voice_mute <peerId>  -- stop a peer transmitting on the radio nets"
                                   : "voice_unmute <peerId>  -- restore a peer's radio transmit";
        registry.registerCommand(name, help, capBit(Capability::Mute),
                                 [ctx, muteVal, name](std::span<std::string_view> args) -> std::string {
                                     if (args.empty())
                                         return std::string("usage: ") + name + " <peerId>";
                                     if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                         return std::string(name) + ": not available";
                                     std::string arg(args[0]);
                                     if (!fl::isAllDigits(arg))
                                         return std::string(name) + ": expected a peer ID";
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return std::string(name) + ": invalid peer ID";
                                     ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId, muteVal, name]() {
                                         const bool ok = ctx->sim.broadcaster->setPeerVoiceMuted(peerId, muteVal);
                                         char m[80];
                                         std::snprintf(m, sizeof(m), "[admin] %s peer %u%s", name, peerId,
                                                       ok ? "" : " (unknown peer)");
                                         printAdmin(*ctx, m);
                                     });
                                     char buf[80];
                                     std::snprintf(buf, sizeof(buf), "%s: queued peer %u", name, peerId);
                                     return std::string(buf);
                                 });
    }

    // voice  -- the radio-net table + who is voice-muted, so an operator can see what the clients see
    registry.registerCommand(
        "voice", "voice  -- list the radio nets and voice-muted peers", 0,
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "voice: not available";
            ctx->sim.gameLoop->enqueueSimCallback([ctx]() {
                const auto& b = *ctx->sim.broadcaster;
                std::string out = "[admin] voice: ";
                out += b.voiceEnabled() ? "enabled" : "DISABLED";
                out += "\n";
                const auto& nets = b.radioNets();
                for (std::size_t i = 0; i < nets.size(); ++i) {
                    const auto& n = nets.nets()[i];
                    char line[192];
                    std::snprintf(line, sizeof(line), "  net %zu  %-12s %-10s %s%s range=%.0fm gain=%.2f", i,
                                  n.id.c_str(), radioNetKindName(n.kind), n.positional ? "positional " : "head-locked ",
                                  n.radioEffect ? "radio" : "clean", static_cast<double>(n.rangeM),
                                  static_cast<double>(n.gain));
                    out += line;
                    out += "\n";
                }
                const std::vector<uint32_t> muted = b.voiceMutedPeers();
                out += "  voice-muted: ";
                if (muted.empty())
                    out += "(none)";
                else
                    for (std::size_t i = 0; i < muted.size(); ++i)
                        out += (i ? ", " : "") + std::to_string(muted[i]);
                printAdmin(*ctx, out);
            });
            return "voice: queued";
        });

    // set_role <peerId> <pilot|observer>  -- switch a peer between pilot and spectator without a reconnect (#857)
    registry.registerCommand(
        "set_role", "set_role <peerId> <pilot|observer>  -- switch a peer's role without a reconnect",
        capBit(Capability::GrantRoles), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: set_role <peerId> <pilot|observer>";
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "set_role: not available";
            std::string idArg(args[0]);
            if (!fl::isAllDigits(idArg))
                return "set_role: invalid peer ID";
            uint32_t peerId = 0;
            if (auto [ptr, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), peerId); ec != std::errc{})
                return "set_role: invalid peer ID";
            fl::PeerRole role;
            if (args[1] == "pilot")
                role = fl::PeerRole::Pilot;
            else if (args[1] == "observer")
                role = fl::PeerRole::Observer;
            else
                return "set_role: role must be 'pilot' or 'observer'";
            ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId, role]() {
                // The sync ack below can only say "queued"; the outcome is known here, one tick later.
                // A false return is a real refusal (no airframe — entity soft cap, #1049 — or an
                // unknown peer), so report it rather than claiming a role change that did not happen.
                const bool ok = ctx->sim.broadcaster->setPeerRole(peerId, role);
                char m[112];
                if (!ok)
                    std::snprintf(m, sizeof(m),
                                  "[admin] set_role peer %u -> %s FAILED (peer gone, already in role, "
                                  "or no airframe available)",
                                  peerId, role == fl::PeerRole::Observer ? "observer" : "pilot");
                else
                    std::snprintf(m, sizeof(m), "[admin] set peer %u role to %s", peerId,
                                  role == fl::PeerRole::Observer ? "observer" : "pilot");
                printAdmin(*ctx, m);
            });
            char buf[80];
            std::snprintf(buf, sizeof(buf), "set_role: queued peer %u -> %s", peerId,
                          role == fl::PeerRole::Observer ? "observer" : "pilot");
            return std::string(buf);
        });

    // grant <peerId> <admin|moderator|gm|faction_leader> [factionIndex]  -- grant a peer authority (#947,
    // rung 2 of the #944 grant ladder). Ephemeral (lost on disconnect); requires grant_roles. Re-sends
    // MsgConnectAck so the client's granted-authority TLV updates and GM/mod UI appears (#949).
    registry.registerCommand(
        "grant",
        "grant <peerId> <admin|moderator|gm|faction_leader> [factionIndex]  -- grant a peer a role's "
        "capabilities (ephemeral; lost on disconnect)",
        capBit(Capability::GrantRoles), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: grant <peerId> <admin|moderator|gm|faction_leader> [factionIndex]";
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "grant: not available";
            std::string idArg(args[0]);
            if (!fl::isAllDigits(idArg))
                return "grant: invalid peer ID";
            uint32_t peerId = 0;
            if (auto [p, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), peerId); ec != std::errc{})
                return "grant: invalid peer ID";
            const auto preset = fl::parseRolePreset(args[1]);
            if (!preset)
                return "grant: role must be one of admin, moderator, gm, faction_leader";
            uint16_t faction = fl::PeerAuthority::kNoFactionBinding;
            if (args.size() >= 3) {
                std::string facArg(args[2]);
                unsigned f = 0;
                if (auto [p, ec] = std::from_chars(facArg.data(), facArg.data() + facArg.size(), f);
                    ec != std::errc{} || f > 0xFFFFu)
                    return "grant: invalid faction index";
                faction = static_cast<uint16_t>(f);
            }
            const fl::PeerAuthority authority{*preset, faction};
            std::string roleName(args[1]);
            ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId, authority, roleName]() {
                const bool ok = ctx->sim.broadcaster->setPeerAuthority(peerId, authority);
                char m[96];
                if (ok)
                    std::snprintf(m, sizeof(m), "[admin] granted peer %u role %s", peerId, roleName.c_str());
                else
                    std::snprintf(m, sizeof(m), "[admin] grant failed: peer %u not found", peerId);
                printAdmin(*ctx, m);
            });
            char buf[96];
            std::snprintf(buf, sizeof(buf), "grant: queued peer %u -> %s", peerId, roleName.c_str());
            return std::string(buf);
        });

    // revoke <peerId>  -- clear a peer's granted authority (#947). Requires grant_roles.
    registry.registerCommand(
        "revoke", "revoke <peerId>  -- clear a peer's granted authority", capBit(Capability::GrantRoles),
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: revoke <peerId>";
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "revoke: not available";
            std::string idArg(args[0]);
            if (!fl::isAllDigits(idArg))
                return "revoke: invalid peer ID";
            uint32_t peerId = 0;
            if (auto [p, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), peerId); ec != std::errc{})
                return "revoke: invalid peer ID";
            ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId]() {
                const bool ok = ctx->sim.broadcaster->setPeerAuthority(peerId, fl::PeerAuthority{});
                char m[96];
                if (ok)
                    std::snprintf(m, sizeof(m), "[admin] revoked peer %u authority", peerId);
                else
                    std::snprintf(m, sizeof(m), "[admin] revoke failed: peer %u not found", peerId);
                printAdmin(*ctx, m);
            });
            char buf[64];
            std::snprintf(buf, sizeof(buf), "revoke: queued peer %u", peerId);
            return std::string(buf);
        });

    // team <peerId> <factionIndex>  -- move a peer to a team (#522). Bypasses the balance guard (admin).
    registry.registerCommand(
        "team", "team <peerId> <factionIndex>  -- move a peer to a team (bypasses the balance guard)",
        capBit(Capability::GrantRoles), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: team <peerId> <factionIndex>";
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "team: not available";
            std::string idArg(args[0]);
            std::string facArg(args[1]);
            if (!fl::isAllDigits(idArg) || !fl::isAllDigits(facArg))
                return "team: peerId and factionIndex must be numeric";
            uint32_t peerId = 0;
            unsigned faction = 0;
            if (auto [p, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), peerId); ec != std::errc{})
                return "team: invalid peer ID";
            if (auto [p, ec] = std::from_chars(facArg.data(), facArg.data() + facArg.size(), faction);
                ec != std::errc{} || faction > 0xFFFFu)
                return "team: invalid faction index";
            const uint16_t f = static_cast<uint16_t>(faction);
            ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId, f]() {
                ctx->sim.broadcaster->setPeerFaction(peerId, f);
                char m[80];
                std::snprintf(m, sizeof(m), "[admin] moved peer %u to team %u", peerId, f);
                printAdmin(*ctx, m);
            });
            char buf[80];
            std::snprintf(buf, sizeof(buf), "team: queued peer %u -> team %u", peerId, f);
            return std::string(buf);
        });

    // respawn <peerId>  -- force a dead peer to respawn immediately (#648)
    registry.registerCommand("respawn", "respawn <peerId>  -- force a dead peer to respawn now",
                             capBit(Capability::SpawnAny), [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: respawn <peerId>";
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                     return "respawn: not available";
                                 std::string idArg(args[0]);
                                 if (!fl::isAllDigits(idArg))
                                     return "respawn: invalid peer ID";
                                 uint32_t peerId = 0;
                                 if (auto [p, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), peerId);
                                     ec != std::errc{})
                                     return "respawn: invalid peer ID";
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId]() {
                                     ctx->sim.broadcaster->respawnParticipant(peerId);
                                     char m[64];
                                     std::snprintf(m, sizeof(m), "[admin] respawned peer %u", peerId);
                                     printAdmin(*ctx, m);
                                 });
                                 char buf[64];
                                 std::snprintf(buf, sizeof(buf), "respawn: queued peer %u", peerId);
                                 return std::string(buf);
                             });

    // spectate <peerId> <entityIdx|off>  -- point a dead/observer peer's interest at an entity (#403)
    registry.registerCommand(
        "spectate", "spectate <peerId> <entityIdx|off>  -- lock a dead/observer peer's view onto an entity",
        capBit(Capability::SpectateAny), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: spectate <peerId> <entityIdx|off>";
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "spectate: not available";
            std::string idArg(args[0]);
            if (!fl::isAllDigits(idArg))
                return "spectate: invalid peer ID";
            uint32_t peerId = 0;
            if (auto [p, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), peerId); ec != std::errc{})
                return "spectate: invalid peer ID";
            uint32_t target = 0xFFFFFFFFu; // off
            if (args[1] != "off") {
                std::string tArg(args[1]);
                if (!fl::isAllDigits(tArg))
                    return "spectate: entity index must be a number or 'off'";
                if (auto [p, ec] = std::from_chars(tArg.data(), tArg.data() + tArg.size(), target); ec != std::errc{})
                    return "spectate: invalid entity index";
            }
            ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId, target]() {
                const bool ok = ctx->sim.broadcaster->setSpectateTarget(peerId, target);
                char m[80];
                if (target == 0xFFFFFFFFu)
                    std::snprintf(m, sizeof(m), "[admin] spectate off for peer %u%s", peerId, ok ? "" : " (unknown)");
                else
                    std::snprintf(m, sizeof(m), "[admin] peer %u spectating entity %u%s", peerId, target,
                                  ok ? "" : " (unknown)");
                printAdmin(*ctx, m);
            });
            char buf[64];
            std::snprintf(buf, sizeof(buf), "spectate: queued peer %u", peerId);
            return std::string(buf);
        });

    // seats <entityIdx>  -- inspect a crewed aircraft's seat roster/occupancy (#974)
    registry.registerCommand("seats", "seats <entityIdx>  -- show a crewed aircraft's seat roster and occupancy", 0,
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: seats <entityIdx>";
                                 if (!ctx->sim.broadcaster)
                                     return "seats: not available";
                                 std::string idArg(args[0]);
                                 if (!fl::isAllDigits(idArg))
                                     return "seats: invalid entity index";
                                 uint32_t idx = 0;
                                 if (auto [ptr, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), idx);
                                     ec != std::errc{})
                                     return "seats: invalid entity index";
                                 return ctx->sim.broadcaster->crewRosterText(idx);
                             });

    // set_seat <entityIdx> <seat> <peerId|bot|empty>  -- force a non-fly seat's occupancy (#974)
    registry.registerCommand(
        "set_seat", "set_seat <entityIdx> <seat> <peerId|bot|empty>  -- force a non-fly seat's occupancy",
        capBit(Capability::GrantRoles), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 3)
                return "usage: set_seat <entityIdx> <seat> <peerId|bot|empty>";
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "set_seat: not available";
            std::string idArg(args[0]), seatArg(args[1]);
            if (!fl::isAllDigits(idArg) || !fl::isAllDigits(seatArg))
                return "set_seat: entity index and seat must be integers";
            uint32_t entityIdx = 0, seat = 0;
            if (auto [p, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), entityIdx); ec != std::errc{})
                return "set_seat: invalid entity index";
            if (auto [p, ec] = std::from_chars(seatArg.data(), seatArg.data() + seatArg.size(), seat);
                ec != std::errc{})
                return "set_seat: invalid seat";
            if (seat > 255u)
                return "set_seat: seat out of range";
            fl::SeatOccupancy occ = fl::SeatOccupancy::Bot;
            uint32_t peerId = 0;
            if (args[2] == "bot")
                occ = fl::SeatOccupancy::Bot;
            else if (args[2] == "empty")
                occ = fl::SeatOccupancy::Empty;
            else {
                std::string peerArg(args[2]);
                if (!fl::isAllDigits(peerArg))
                    return "set_seat: third arg must be a peerId, 'bot', or 'empty'";
                if (auto [p, ec] = std::from_chars(peerArg.data(), peerArg.data() + peerArg.size(), peerId);
                    ec != std::errc{})
                    return "set_seat: invalid peerId";
                occ = fl::SeatOccupancy::Human;
            }
            const auto seat8 = static_cast<uint8_t>(seat);
            ctx->sim.gameLoop->enqueueSimCallback([ctx, entityIdx, seat8, occ, peerId]() {
                const std::string err = ctx->sim.broadcaster->adminSetSeat(entityIdx, seat8, occ, peerId);
                char m[128];
                if (err.empty())
                    std::snprintf(m, sizeof(m), "[admin] set entity %u seat %u -> %s", entityIdx, seat8,
                                  occ == fl::SeatOccupancy::Human ? "human"
                                  : occ == fl::SeatOccupancy::Bot ? "bot"
                                                                  : "empty");
                else
                    std::snprintf(m, sizeof(m), "[admin] %s", err.c_str());
                printAdmin(*ctx, m);
            });
            char buf[96];
            std::snprintf(buf, sizeof(buf), "set_seat: queued entity %u seat %u", entityIdx, seat8);
            return std::string(buf);
        });

    // ban <peerId|IP>
    registry.registerCommand("ban", "ban <peerId|IP>  -- add IP to in-memory ban list and kick matching peers",
                             capBit(Capability::KickBan), [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: ban <peerId|IP>";
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                     return "ban: not available";
                                 std::string arg(args[0]);
                                 if (fl::isAllDigits(arg)) {
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return "ban: invalid peer ID";
                                     ctx->sim.gameLoop->enqueueSimCallback([ctx, peerId]() {
                                         std::string foundIp;
                                         ctx->sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                                             if (pi.peerId == peerId)
                                                 foundIp = extractIp(pi.addr);
                                         });
                                         char m[128];
                                         if (foundIp.empty()) {
                                             std::snprintf(m, sizeof(m), "[admin] ban: peer %u not found", peerId);
                                         } else {
                                             ctx->sim.broadcaster->banAddress(foundIp);
                                             if (ctx->bans.saveBanlist)
                                                 ctx->bans.saveBanlist(ctx->sim.broadcaster->getBannedAddresses());
                                             std::snprintf(m, sizeof(m), "[admin] banned IP %s (peer %u)",
                                                           foundIp.c_str(), peerId);
                                         }
                                         printAdmin(*ctx, m);
                                     });
                                     char banBuf[64];
                                     std::snprintf(banBuf, sizeof(banBuf), "ban: queued for peer %u", peerId);
                                     return std::string(banBuf);
                                 } else {
                                     std::string ip = normalizeIp(arg);
                                     ctx->sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                                         ctx->sim.broadcaster->banAddress(ip);
                                         if (ctx->bans.saveBanlist)
                                             ctx->bans.saveBanlist(ctx->sim.broadcaster->getBannedAddresses());
                                         char m[128];
                                         std::snprintf(m, sizeof(m), "[admin] banned IP %s", ip.c_str());
                                         printAdmin(*ctx, m);
                                     });
                                     return "ban: banning IP " + ip;
                                 }
                             });

    // unban <IP>
    registry.registerCommand("unban", "unban <IP>  -- remove an IP from the in-memory ban list",
                             capBit(Capability::KickBan), [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: unban <IP>";
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                     return "unban: not available";
                                 std::string ip = normalizeIp(args[0]);
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                                     ctx->sim.broadcaster->unbanAddress(ip);
                                     if (ctx->bans.saveBanlist)
                                         ctx->bans.saveBanlist(ctx->sim.broadcaster->getBannedAddresses());
                                     char m[128];
                                     std::snprintf(m, sizeof(m), "[admin] unbanned IP %s", ip.c_str());
                                     printAdmin(*ctx, m);
                                 });
                                 return "unban: unbanning IP " + ip;
                             });

    // admin_unlock <IP>
    registry.registerCommand(
        "admin_unlock", "admin_unlock <IP>  -- clear the auth lockout for an IP on every admin channel",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: admin_unlock <IP>";
            if (!ctx->adminChannels || !ctx->sim.gameLoop)
                return "admin_unlock: not available";
            std::string ip = normalizeIp(args[0]);
            ctx->sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                // Every registered channel, whatever it is (#1079). This used to name three by hand and
                // its own comment admitted that unlocking two of three "would be worse than not
                // unlocking at all" -- a fourth frontend would have been silently skipped.
                const std::vector<std::string> cleared = ctx->adminChannels->clearLockoutEverywhere(ip);
                char m[256];
                if (!cleared.empty()) {
                    // The channels that ACTUALLY held a lockout, not the fixed list of wired ones: the
                    // operator is told what changed.
                    std::string names;
                    for (const std::string& n : cleared) {
                        if (!names.empty())
                            names += " + ";
                        names += n;
                    }
                    std::snprintf(m, sizeof(m), "[admin] unlocked %s (%s)", ip.c_str(), names.c_str());
                } else {
                    std::snprintf(m, sizeof(m), "[admin] admin_unlock: %s was not locked on any channel", ip.c_str());
                }
                printAdmin(*ctx, m);
            });
            return "admin_unlock: queued for " + ip;
        });

    // admin_auth_status
    registry.registerCommand(
        "admin_auth_status", "admin_auth_status  -- show per-IP auth lockout state for every admin channel",
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx->adminChannels)
                return "admin_auth_status: not available";
            const auto& channels = ctx->adminChannels->channels();
            if (channels.empty())
                return "[admin] no admin channels registered";
            // Registration order, one section per channel. Nothing here knows what the channels ARE,
            // which is the property that makes a seventh frontend visible for free.
            std::string detail;
            for (const fl::AdminChannel* c : channels) {
                if (!detail.empty())
                    detail += "\n\n";
                detail += formatAuthSection(c->name() + " channel", c->authSummary(), c->hasPerIpAuth());
            }
            std::printf("%s\n", detail.c_str());
            std::fflush(stdout);
            return detail;
        });

    // set_weather <preset>
    registry.registerCommand(
        "set_weather", "set_weather <clear|partly_cloudy|overcast|rain|storm|snow|blizzard>  -- change weather preset",
        capBit(Capability::ServerConfig), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: set_weather <clear|partly_cloudy|overcast|rain|storm|snow|blizzard>";
            if (!ctx->sim.weatherController || !ctx->sim.gameLoop)
                return "set_weather: not available";
            fl::WeatherPreset preset;
            if (args[0] == "clear")
                preset = fl::WeatherPreset::Clear;
            else if (args[0] == "partly_cloudy")
                preset = fl::WeatherPreset::PartlyCloudy;
            else if (args[0] == "overcast")
                preset = fl::WeatherPreset::Overcast;
            else if (args[0] == "rain")
                preset = fl::WeatherPreset::Rain;
            else if (args[0] == "storm")
                preset = fl::WeatherPreset::Storm;
            else if (args[0] == "snow")
                preset = fl::WeatherPreset::Snow;
            else if (args[0] == "blizzard")
                preset = fl::WeatherPreset::Blizzard;
            else
                return "set_weather: unknown preset (clear|partly_cloudy|overcast|rain|storm|snow|blizzard)";
            ctx->sim.gameLoop->enqueueSimCallback([ctx, preset]() { ctx->sim.weatherController->setPreset(preset); });
            return std::string("set_weather: ") + std::string(args[0]);
        });

    // set_time <hours>
    registry.registerCommand("set_time", "set_time <0-24>  -- set in-game time of day (hours, float)",
                             capBit(Capability::ServerConfig), [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: set_time <0-24>";
                                 // Validate argument before context check so parse/range errors are always reported.
                                 const auto parsedHours = fl::parseFloat(args[0]);
                                 if (!parsedHours)
                                     return "set_time: invalid value";
                                 const float hours = *parsedHours;
                                 if (hours < 0.f || hours > 24.f)
                                     return "set_time: value must be in [0, 24]";
                                 if (!ctx->sim.weatherController || !ctx->sim.gameLoop)
                                     return "set_time: not available";
                                 ctx->sim.gameLoop->enqueueSimCallback(
                                     [ctx, hours]() { ctx->sim.weatherController->setTimeOfDay(hours); });
                                 char buf[64];
                                 std::snprintf(buf, sizeof(buf), "set_time: %.2f", hours);
                                 return buf;
                             });

    // ── Air-traffic control (#705) ───────────────────────────────────────────
    registry.registerCommand("atc_status", "atc_status [airport]  -- show ATC facility queues and runway occupancy", 0,
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (!ctx->sim.atc)
                                     return "atc_status: ATC not available";
                                 const std::string filter = args.empty() ? std::string{} : std::string(args[0]);
                                 return ctx->sim.atc->statusText(filter);
                             });

    registry.registerCommand(
        "atc_scramble", "atc_scramble <airport> <type> [count]  -- launch AI departures from a named airport",
        capBit(Capability::CommandAnyAi), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: atc_scramble <airport> <type> [count]";
            if (!ctx->sim.atc || !ctx->sim.gameLoop)
                return "atc_scramble: ATC not available";
            std::string airport(args[0]);
            std::string type(args[1]);
            int count = 1;
            if (args.size() >= 3) {
                std::string c(args[2]);
                char* end = nullptr;
                long v = std::strtol(c.c_str(), &end, 10);
                if (end == c.c_str() || *end != '\0' || v < 1 || v > 64)
                    return "atc_scramble: count must be in [1, 64]";
                count = static_cast<int>(v);
            }
            ctx->sim.gameLoop->enqueueSimCallback([ctx, airport, type, count]() {
                const bool ok = ctx->sim.atc->scramble(airport, type, count);
                printAdmin(*ctx, ok ? "[atc] scramble launched" : "[atc] scramble failed (unknown airport?)");
            });
            char buf[96];
            std::snprintf(buf, sizeof(buf), "atc_scramble: queued %d from %s", count, airport.c_str());
            return std::string(buf);
        });

    registry.registerCommand("atc_hold", "atc_hold <airport> <on|off>  -- freeze or release departures at an airport",
                             capBit(Capability::CommandAnyAi), [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.size() < 2)
                                     return "usage: atc_hold <airport> <on|off>";
                                 if (!ctx->sim.atc || !ctx->sim.gameLoop)
                                     return "atc_hold: ATC not available";
                                 bool hold;
                                 if (args[1] == "on")
                                     hold = true;
                                 else if (args[1] == "off")
                                     hold = false;
                                 else
                                     return "atc_hold: second argument must be on|off";
                                 std::string airport(args[0]);
                                 ctx->sim.gameLoop->enqueueSimCallback(
                                     [ctx, airport, hold]() { ctx->sim.atc->holdDepartures(airport, hold); });
                                 return std::string("atc_hold: ") + (hold ? "holding " : "releasing ") + airport;
                             });

    // spawn <type> <x> <y> <z> [--faction <n>] [--ai <behavior> [behavior-args...]]
    registry.registerCommand(
        "spawn",
        "spawn <type> <x> <y> <z> [--faction <n>] [--ai <behavior> [args...]]  -- spawn entity with optional "
        "faction and AI controller",
        capBit(Capability::SpawnAny), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 4)
                return "usage: spawn <type> <x> <y> <z> [--faction <n>] [--ai <behavior> [args...]]";
            if (!ctx->sim.entityManager || !ctx->sim.gameLoop)
                return "spawn: not available";
            std::string typeId(args[0]);
            double x = 0, y = 0, z = 0;
            // Double, matching tp: this used to round coordinates through a float, which at planet
            // scale (~6.4e6 m) landed a spawn about half a metre from where tp would have put it.
            auto parseD = [](std::string_view s, double& out) { return fl::readInto(fl::parseDouble(s), out); };
            if (!parseD(args[1], x) || !parseD(args[2], y) || !parseD(args[3], z))
                return "spawn: invalid coordinates";

            // Pull --faction <n> out first (order-independent), leaving the rest for --ai parsing.
            // Faction 0 = neutral (default); factions are server-assigned at spawn time (#465).
            uint16_t factionIndex = 0;
            std::vector<std::string_view> rest;
            for (std::size_t i = 4; i < args.size(); ++i) {
                if (args[i] == "--faction") {
                    if (i + 1 >= args.size())
                        return "spawn: --faction requires a number";
                    uint32_t f = 0;
                    std::string_view fv = args[++i];
                    auto [ptr, ec] = std::from_chars(fv.data(), fv.data() + fv.size(), f);
                    if (ec != std::errc{} || ptr != fv.data() + fv.size() || f > 0xFFFFu)
                        return "spawn: --faction must be an integer in [0, 65535]";
                    factionIndex = static_cast<uint16_t>(f);
                } else {
                    rest.push_back(args[i]);
                }
            }

            // Parse optional --ai <behavior> [behavior-args...] from the remaining tokens.
            std::string behavior;
            std::vector<std::string> behaviorArgStrings;
            for (std::size_t i = 0; i < rest.size(); ++i) {
                if (rest[i] == "--ai") {
                    if (i + 1 >= rest.size())
                        return "spawn: --ai requires a behavior name";
                    behavior = std::string(rest[++i]);
                    while (i + 1 < rest.size())
                        behaviorArgStrings.emplace_back(rest[++i]);
                    break;
                }
            }

            // Resolve Lua AI script bytes on the dispatch thread (main or sim thread).
            // ctx->env.loadAIScript reads from a pre-loaded read-only cache — safe from any thread.
            std::string luaScriptSrc;
            fl::ScriptPackSource luaScriptPack;
            std::string effectiveBehavior = behavior; // may change to "lua" from aiScriptAsset auto-detect

            if (behavior == "lua") {
                if (!ctx->env.loadAIScript)
                    return "spawn: --ai lua: Lua AI scripting not available";
                std::string scriptName = behaviorArgStrings.empty() ? "" : behaviorArgStrings[0];
                if (scriptName.empty())
                    return "spawn: --ai lua requires a script name";
                auto [src, pack] = ctx->env.loadAIScript(scriptName);
                if (src.empty()) {
                    char em[128];
                    std::snprintf(em, sizeof(em), "spawn: --ai lua: script '%s' not found", scriptName.c_str());
                    return std::string(em);
                }
                luaScriptSrc = std::move(src);
                luaScriptPack = std::move(pack);
            } else if (behavior.empty() && ctx->sim.typeRegistry && ctx->env.loadAIScript) {
                // Auto-detect: check if the entity type has a default AI script.
                const fl::EntityDef* def = ctx->sim.typeRegistry->findById(typeId.c_str());
                if (def && !def->aiScriptAsset.empty()) {
                    auto [src, pack] = ctx->env.loadAIScript(def->aiScriptAsset);
                    if (!src.empty()) {
                        luaScriptSrc = std::move(src);
                        luaScriptPack = std::move(pack);
                        effectiveBehavior = "lua:" + def->aiScriptAsset;
                    }
                }
            }

            ctx->sim.gameLoop->enqueueSimCallback([ctx, typeId, x, y, z, factionIndex, behavior, behaviorArgStrings,
                                                   luaScriptSrc, luaScriptPack]() {
                fl::EntityTransform t{};
                t.pos[0] = x;
                t.pos[1] = y;
                t.pos[2] = z;
                fl::EntityId id = ctx->sim.entityManager->spawn(typeId.c_str(), t);
                char m[160];
                if (id.valid()) {
                    // Faction is server-assigned at spawn time (#465); 0 = neutral (default).
                    if (factionIndex != 0) {
                        if (fl::EntityState* s = ctx->sim.entityManager->get(id))
                            s->factionIndex = factionIndex;
                    }
                    std::snprintf(m, sizeof(m), "[admin] spawned %s entity=%u/%u", typeId.c_str(), id.index,
                                  id.generation);
                    printAdmin(*ctx, m);

                    if (ctx->sim.broadcaster) {
                        // The shared construction ladder (#1236); this path has the ATC service, so
                        // an admin-spawned script reaches atc.* (#705).
                        fl::AiControllerRequest req;
                        req.luaSource = luaScriptSrc;
                        req.luaPack = luaScriptPack;
                        req.behavior = behavior;
                        req.args = behaviorArgStrings;
                        req.entityManager = ctx->sim.entityManager;
                        req.worldApi = ctx->sim.worldApi;
                        req.atcService = ctx->sim.atc;
                        auto built = fl::buildAiController(req);
                        std::unique_ptr<fl::IEntityController> ctrl = std::move(built.controller);

                        if (built.error == fl::AiBuildError::LuaScriptError) {
                            char em[192];
                            std::snprintf(em, sizeof(em), "[admin] spawn: Lua script error: %s", built.detail.c_str());
                            printAdmin(*ctx, em);
                        } else if (built.error == fl::AiBuildError::UnknownBehavior) {
                            char wm[128];
                            std::snprintf(wm, sizeof(wm), "[admin] spawn: unknown AI behavior '%s' or bad args",
                                          behavior.c_str());
                            printAdmin(*ctx, wm);
                        }

                        if (ctrl) {
                            ctx->sim.broadcaster->registerController(id, std::move(ctrl));
                            char am[128];
                            std::snprintf(am, sizeof(am), "[admin] attached AI '%s' to entity=%u",
                                          behavior.empty() ? "lua(auto)" : behavior.c_str(), id.index);
                            printAdmin(*ctx, am);
                        }
                    }
                } else {
                    std::snprintf(m, sizeof(m), "[admin] spawn: type '%s' unknown or cap reached", typeId.c_str());
                    printAdmin(*ctx, m);
                }
            });

            char spawnBuf[160];
            if (effectiveBehavior.empty()) {
                std::snprintf(spawnBuf, sizeof(spawnBuf), "spawn: queued type %s at %.1f %.1f %.1f", typeId.c_str(),
                              static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            } else {
                std::snprintf(spawnBuf, sizeof(spawnBuf), "spawn: queued type %s at %.1f %.1f %.1f --ai %s",
                              typeId.c_str(), static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                              effectiveBehavior.c_str());
            }
            return std::string(spawnBuf);
        });

    // kill <idx>
    registry.registerCommand(
        "kill", "kill <idx>  -- remove a live entity by pool index (see 'peers' or 'entities')",
        capBit(Capability::SpawnAny), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: kill <idx>";
            if (!ctx->sim.entityManager || !ctx->sim.gameLoop)
                return "kill: not available";
            uint32_t targetIdx = 0;
            auto [ptr, ec] = std::from_chars(args[0].data(), args[0].data() + args[0].size(), targetIdx);
            if (ec != std::errc{})
                return "kill: invalid index";
            ctx->sim.gameLoop->enqueueSimCallback([ctx, targetIdx]() {
                fl::EntityId killId;
                ctx->sim.entityManager->forEach([&](const fl::EntityState& state) {
                    if (!killId.valid() && state.id.index == targetIdx)
                        killId = state.id;
                });
                char m[128];
                if (killId.valid()) {
                    ctx->sim.entityManager->kill(killId);
                    std::snprintf(m, sizeof(m), "[admin] killed entity %u/%u", killId.index, killId.generation);
                } else {
                    std::snprintf(m, sizeof(m), "[admin] kill: no live entity with index %u", targetIdx);
                }
                printAdmin(*ctx, m);
            });
            char killBuf[64];
            std::snprintf(killBuf, sizeof(killBuf), "kill: queued index %u", targetIdx);
            return std::string(killBuf);
        });

    // tp <idx> <x> <y> <z>
    registry.registerCommand(
        "tp", "tp <idx> <x> <y> <z>  -- teleport entity to world position", capBit(Capability::SpawnAny),
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 4)
                return "usage: tp <idx> <x> <y> <z>";
            if (!ctx->sim.entityManager || !ctx->sim.gameLoop)
                return "tp: not available";
            uint32_t targetIdx = 0;
            auto [ptr, ec] = std::from_chars(args[0].data(), args[0].data() + args[0].size(), targetIdx);
            if (ec != std::errc{})
                return "tp: invalid entity index";
            // Parse coordinates with strtod (from_chars for double not on Apple Clang).
            auto parseCoord = [](std::string_view sv, double& out) -> bool {
                return fl::readInto(fl::parseDouble(sv), out);
            };
            double x{}, y{}, z{};
            if (!parseCoord(args[1], x) || !parseCoord(args[2], y) || !parseCoord(args[3], z))
                return "tp: invalid coordinates";
            ctx->sim.gameLoop->enqueueSimCallback([ctx, targetIdx, x, y, z]() {
                ctx->sim.entityManager->forEach([&](fl::EntityState& state) {
                    if (state.id.index == targetIdx) {
                        state.transform.pos[0] = x;
                        state.transform.pos[1] = y;
                        state.transform.pos[2] = z;
                        char m[128];
                        std::snprintf(m, sizeof(m), "[admin] teleported entity %u/%u to X:%+.1f Y:%+.1f Z:%+.1f",
                                      state.id.index, state.id.generation, static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(z));
                        printAdmin(*ctx, m);
                    }
                });
            });
            char tpBuf[64];
            std::snprintf(tpBuf, sizeof(tpBuf), "tp: queued entity %u to %.1f %.1f %.1f", targetIdx,
                          static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            return std::string(tpBuf);
        });

    // detonate (#356) — a warhead with no weapon: testing, ops, and scripted events.
    registry.registerCommand(
        "detonate",
        "detonate <x> <y> <z> <blast_radius_m> <damage> [--nuclear]  -- AoE warhead at a world "
        "position; --nuclear adds the EMP ring (avionics kill) at 4x the blast radius",
        capBit(Capability::SpawnAny), [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 5)
                return "usage: detonate <x> <y> <z> <blast_radius_m> <damage> [--nuclear]";
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "detonate: not available";
            auto parseNum = [](std::string_view sv, double& out) { return fl::readInto(fl::parseDouble(sv), out); };
            double x{}, y{}, z{}, radius{}, damage{};
            if (!parseNum(args[0], x) || !parseNum(args[1], y) || !parseNum(args[2], z) || !parseNum(args[3], radius) ||
                !parseNum(args[4], damage))
                return "detonate: invalid arguments";
            if (radius <= 0.0 || damage <= 0.0)
                return "detonate: blast_radius_m and damage must be > 0";
            const bool nuclear = args.size() > 5 && args[5] == "--nuclear";

            ctx->sim.gameLoop->enqueueSimCallback([ctx, x, y, z, radius, damage, nuclear]() {
                fl::BlastSpec blast;
                blast.radiusM = static_cast<float>(radius);
                blast.damage = static_cast<float>(damage);
                blast.nuclear = nuclear;
                const double pos[3] = {x, y, z};
                const fl::WarheadResult r = ctx->sim.broadcaster->applyWarheadAt(pos, blast, fl::EntityId::null());
                char m[160];
                std::snprintf(m, sizeof(m),
                              "[admin] detonated %.0f dmg / %.0f m%s at X:%+.1f Y:%+.1f Z:%+.1f — "
                              "%d damaged, %d EMPed",
                              damage, radius, nuclear ? " (nuclear)" : "", x, y, z, r.damaged, r.emped);
                printAdmin(*ctx, m);
            });
            char buf[96];
            std::snprintf(buf, sizeof(buf), "detonate: queued %.0f dmg / %.0f m%s", damage, radius,
                          nuclear ? " (nuclear)" : "");
            return std::string(buf);
        });

    // reload_config
    registry.registerCommand(
        "reload_config",
        "reload_config  -- re-read server.toml, apply every hot-reloadable key and NAME the"
        " restart-only keys whose values changed (see the reload matrix in server-config.md)",
        capBit(Capability::ServerConfig), [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx->env.configPath || ctx->env.configPath->empty())
                return "reload_config: not available";
            if (!ctx->env.runningConfig)
                return "reload_config: not available (no running config)";
            std::ifstream f(*ctx->env.configPath);
            if (!f)
                return "reload_config: cannot open " + *ctx->env.configPath;
            std::ostringstream ss;
            ss << f.rdbuf();
            // A parse error must not reach the appliers: parseServerConfig answers a syntax error with
            // a DEFAULT config, and applying that would silently reset the live MOTD, draw distance
            // and congestion levers because the operator mistyped a bracket.
            bool parseFailed = false;
            const auto incoming =
                std::make_shared<ServerConfig>(parseServerConfig(ss.str(), ctx->env.logger, &parseFailed));
            if (parseFailed)
                return "reload_config: " + *ctx->env.configPath +
                       " has a TOML syntax error (see the log) -- nothing was applied";

            // Resolve the difficulty preset OFF the sim thread (it may read data/difficulty.toml
            // through the AssetManager); only the resulting POD crosses into the callback.
            const auto scaling = std::make_shared<std::optional<fl::AiScaling>>();
            if (ctx->env.resolveAiScaling)
                *scaling = ctx->env.resolveAiScaling(incoming->ai.difficulty);

            // The diff is computed HERE, on the calling thread, so the operator gets it in the
            // synchronous response rather than a tick later on stdout. It reads two plain configs and
            // touches nothing live.
            const ServerConfig& running = *ctx->env.runningConfig;
            const std::vector<fl::ConfigKeyDiff> ignored = fl::restartOnlyDiffs(running, *incoming);

            if (ctx->sim.broadcaster && ctx->sim.gameLoop) {
                ctx->sim.gameLoop->enqueueSimCallback([ctx, incoming, scaling]() {
                    const fl::ReloadApplyContext rc{*ctx, *incoming, *ctx->env.runningConfig, *scaling};
                    fl::applyHotKeys(rc);
                });
            }

            // Every hot key is applied unconditionally, as it always was; what is reported is what
            // CHANGED, because a list of 26 key names an operator did not edit is not information.
            std::string out = "reload_config: applied " + std::to_string(fl::hotKeyCount()) + " hot key(s)";
            const std::vector<fl::ConfigKeyDiff> changedHot = fl::changedHotKeys(running, *incoming);
            if (!changedHot.empty()) {
                out += "\n  changed: ";
                bool first = true;
                for (const auto& d : changedHot) {
                    if (!first)
                        out += ", ";
                    first = false;
                    out += std::string(d.key) + " " + d.running + " -> " + d.incoming;
                }
            }
            if (!ignored.empty()) {
                // The half an operator used to get silence for. Naming the key AND both values is the
                // difference between "nothing happened" and "this needs a restart to take effect".
                out += "\n  restart required for " + std::to_string(ignored.size()) + " changed key(s): ";
                bool first = true;
                for (const auto& d : ignored) {
                    if (!first)
                        out += ", ";
                    first = false;
                    out += std::string(d.key) + " " + d.running + " -> " + d.incoming;
                }
            }
            return out;
        });

    // reload_banlist
    registry.registerCommand("reload_content",
                             "reload_content  -- evict content caches and live-apply flight-model changes (#152)",
                             capBit(Capability::ServerConfig), [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->env.reloadContent || !ctx->sim.gameLoop)
                                     return "reload_content: not available";
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx]() {
                                     ctx->env.reloadContent();
                                     printAdmin(*ctx, "[admin] reload_content: applied");
                                 });
                                 return "reload_content: queued";
                             });

    registry.registerCommand("reload_banlist",
                             "reload_banlist  -- reload ban list from security.banlist_path in server.toml",
                             capBit(Capability::ServerConfig), [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->bans.banlistPath || ctx->bans.banlistPath->empty())
                                     return "reload_banlist: not available (security.banlist_path not configured)";
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop || !ctx->bans.loadBanlist)
                                     return "reload_banlist: not available";
                                 auto banned = ctx->bans.loadBanlist();
                                 auto count = banned.size();
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx, b = std::move(banned)]() mutable {
                                     ctx->sim.broadcaster->setBannedAddresses(std::move(b));
                                     printAdmin(*ctx, "[admin] reload_banlist: applied");
                                 });
                                 char buf[128];
                                 std::snprintf(buf, sizeof(buf), "reload_banlist: loading %zu IPs from %s", count,
                                               ctx->bans.banlistPath->c_str());
                                 return buf;
                             });

    // reload_allowlist
    registry.registerCommand("reload_allowlist",
                             "reload_allowlist  -- reload allowlist from security.allowlist_path in server.toml",
                             capBit(Capability::ServerConfig), [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->bans.allowlistPath || ctx->bans.allowlistPath->empty())
                                     return "reload_allowlist: not available (security.allowlist_path not configured)";
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop || !ctx->bans.loadAllowlist)
                                     return "reload_allowlist: not available";
                                 auto allowed = ctx->bans.loadAllowlist();
                                 auto count = allowed.size();
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx, a = std::move(allowed)]() mutable {
                                     ctx->sim.broadcaster->setAllowedAddresses(std::move(a));
                                     printAdmin(*ctx, "[admin] reload_allowlist: applied");
                                 });
                                 char buf[128];
                                 std::snprintf(buf, sizeof(buf), "reload_allowlist: loading %zu IPs from %s", count,
                                               ctx->bans.allowlistPath->c_str());
                                 return buf;
                             });

    // trace_start / trace_stop -- server-side input tracing (#560). Toggles recording of every
    // peer's accepted MsgClientInput to per-peer FLIT traces for bot_swarm `--pattern trace:` replay.
    registry.registerCommand(
        "trace_start", "trace_start [dir]  -- start recording peer inputs to FLIT traces (default: configured dir)",
        capBit(Capability::ServerConfig), [ctx](std::span<std::string_view> args) -> std::string {
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "trace_start: not available";
            std::string dir;
            if (!args.empty())
                dir = std::string(args[0]);
            else if (!ctx->env.traceDir.empty())
                dir = ctx->env.traceDir;
            else
                dir = "traces";
            ctx->sim.gameLoop->enqueueSimCallback([ctx, dir]() {
                ctx->sim.broadcaster->setInputTraceDir(dir);
                char m[256];
                std::snprintf(m, sizeof(m), "[admin] trace_start: recording peer inputs to %s", dir.c_str());
                printAdmin(*ctx, m);
            });
            char buf[256];
            std::snprintf(buf, sizeof(buf), "trace_start: recording peer inputs to %s", dir.c_str());
            return buf;
        });

    registry.registerCommand("trace_stop", "trace_stop  -- stop input tracing and close all trace files",
                             capBit(Capability::ServerConfig), [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                                     return "trace_stop: not available";
                                 ctx->sim.gameLoop->enqueueSimCallback([ctx]() {
                                     ctx->sim.broadcaster->setInputTraceDir("");
                                     printAdmin(*ctx, "[admin] trace_stop: input tracing stopped");
                                 });
                                 return "trace_stop: input tracing stopped";
                             });

    // shutdown
    registry.registerCommand(
        "shutdown",
        "shutdown [--in <dur>] [--interval <dur>] [--delay <dur>] [--cancel] [--now] [--force]"
        " [--reason <text>]"
        "  -- schedule/cancel fl-server graceful shutdown with countdown notices;"
        " --reason prepends custom text to each broadcast (stops consuming at next -- flag)",
        capBit(Capability::ServerConfig), [ctx](std::span<std::string_view> args) -> std::string {
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop)
                return "shutdown: not available";

            // Parse flags.
            bool flagCancel = false, flagNow = false, flagForce = false;
            std::optional<uint32_t> flagIn;
            std::optional<uint32_t> flagInterval;
            std::optional<uint32_t> flagDelay;
            std::string flagReason;

            for (std::size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--cancel") {
                    flagCancel = true;
                } else if (args[i] == "--now") {
                    flagNow = true;
                } else if (args[i] == "--force") {
                    flagForce = true;
                } else if (args[i] == "--in") {
                    if (i + 1 >= args.size())
                        return "shutdown: --in requires a duration (e.g. 30m, 60s, 1h30m)";
                    flagIn = parseDurationSecs(args[++i]);
                    if (!flagIn)
                        return "shutdown: invalid duration for --in";
                } else if (args[i] == "--interval") {
                    if (i + 1 >= args.size())
                        return "shutdown: --interval requires a duration";
                    flagInterval = parseDurationSecs(args[++i]);
                    if (!flagInterval)
                        return "shutdown: invalid duration for --interval";
                } else if (args[i] == "--delay") {
                    if (i + 1 >= args.size())
                        return "shutdown: --delay requires a duration";
                    flagDelay = parseDurationSecs(args[++i]);
                    if (!flagDelay)
                        return "shutdown: invalid duration for --delay";
                } else if (args[i] == "--reason") {
                    std::string parts;
                    while (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
                        ++i;
                        if (!parts.empty())
                            parts += ' ';
                        parts += args[i];
                    }
                    if (parts.empty())
                        return "shutdown: --reason requires a value";
                    flagReason = std::move(parts);
                } else {
                    return "shutdown: unknown flag: " + std::string(args[i]);
                }
            }

            // No args → show status (enqueue sim-thread read).
            if (!flagCancel && !flagNow && !flagIn && !flagDelay) {
                ctx->sim.gameLoop->enqueueSimCallback([ctx]() {
                    char m[128];
                    if (ctx->sim.broadcaster->isShuttingDown()) {
                        uint32_t secs = ctx->sim.broadcaster->secondsUntilShutdown();
                        std::snprintf(m, sizeof(m), "[admin] shutdown scheduled in %u seconds", secs);
                    } else {
                        std::snprintf(m, sizeof(m), "[admin] no shutdown scheduled");
                    }
                    printAdmin(*ctx, m);
                });
                return "shutdown: status queued";
            }

            // --cancel
            if (flagCancel) {
                ctx->sim.gameLoop->enqueueSimCallback([ctx]() { ctx->sim.broadcaster->cancelShutdown(); });
                return "shutdown: cancelled";
            }

            // --delay (push back existing shutdown)
            if (flagDelay) {
                uint32_t extra = *flagDelay;
                ctx->sim.gameLoop->enqueueSimCallback([ctx, extra]() {
                    char m[128];
                    if (!ctx->sim.broadcaster->extendShutdown(extra))
                        std::snprintf(m, sizeof(m), "[admin] shutdown --delay: no active shutdown");
                    else
                        std::snprintf(m, sizeof(m), "[admin] shutdown delayed by %u seconds", extra);
                    printAdmin(*ctx, m);
                });
                return "shutdown: extension queued";
            }

            // --now or --in: confirmation gate.
            if (ctx->shutdown.requireConfirm && !flagForce) {
                if (flagNow)
                    return "Server will shut down immediately. Re-run with --force to confirm.";
                uint32_t secs = *flagIn;
                uint32_t mins = secs / 60;
                char buf[128];
                if (mins > 0)
                    std::snprintf(buf, sizeof(buf),
                                  "Server will shut down in %u minute(s). Re-run with --force to confirm.", mins);
                else
                    std::snprintf(buf, sizeof(buf),
                                  "Server will shut down in %u second(s). Re-run with --force to confirm.", secs);
                return buf;
            }

            // Enforce minimum delay (--now bypasses this).
            if (flagIn && *flagIn < ctx->shutdown.minDelayS) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "shutdown: delay must be at least %u seconds (config min_shutdown_delay_s)",
                              ctx->shutdown.minDelayS);
                return buf;
            }

            // Schedule shutdown.
            uint32_t delaySecs = flagNow ? 0u : *flagIn;
            uint32_t intervalSecs = flagInterval.value_or(ctx->shutdown.warningIntervalS);
            ctx->sim.gameLoop->enqueueSimCallback([ctx, delaySecs, intervalSecs, flagReason]() {
                ctx->sim.broadcaster->initiateShutdown(delaySecs, intervalSecs, flagReason);
            });

            std::string result;
            if (flagNow)
                result = "shutdown: broadcasting immediate shutdown notice...";
            else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "shutdown: scheduled in %u seconds", delaySecs);
                result = buf;
            }
            if (!flagReason.empty() && flagReason.size() > 27)
                result += " (note: reason may be truncated in short-duration notices)";
            return result;
        });

    // pause / resume
    registry.registerCommand("pause", "pause  -- pause the simulation (ticks stop; connections stay active)",
                             capBit(Capability::ServerConfig), [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->sim.gameLoop)
                                     return "pause: game loop not available";
                                 ctx->sim.gameLoop->setRate(TimeRate::Paused);
                                 if (ctx->rcon.shell)
                                     ctx->rcon.shell->print("simulation paused");
                                 return "simulation paused";
                             });

    registry.registerCommand("resume", "resume  -- resume the simulation at normal rate",
                             capBit(Capability::ServerConfig), [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->sim.gameLoop)
                                     return "resume: game loop not available";
                                 ctx->sim.gameLoop->setRate(TimeRate::Normal);
                                 if (ctx->rcon.shell)
                                     ctx->rcon.shell->print("simulation resumed");
                                 return "simulation resumed";
                             });

    // quit
    // -------------------------------------------------------------------------------------------
    // flight  -- the formation / command-hierarchy surface (#610)
    //
    // This is the GAME MASTER and AWACS path. The wire order channel (MsgWingmanCommand) is
    // authorized by commanding a formation; the console is authorized by the operator password, so
    // it can build and order formations it is not part of. Both dispatch through the SAME code
    // (WorldBroadcaster::dispatchOrder), so a console order and a radio order cannot behave
    // differently — which is the property that makes this the future MCP-allowlistable surface too.
    // -------------------------------------------------------------------------------------------
    registry.registerCommand(
        "flight",
        "flight list | create <anchorIdx> [--commander <peerId>] [--parent <id>] [--callsign <name>]\n"
        "       | add <id> <entityIdx> [slot] | order <id> <command> [--member <idx>] [--target <idx>] "
        "[--cascade]\n"
        "       | disband <id>   -- formations and the chain of command; `order` commands are the six "
        "wingman commands (attack_my_target, engage_bandits, rejoin, cover_me, hold_fire, return_to_base)",
        capBit(Capability::CommandAnyAi), [ctx](std::span<std::string_view> args) -> std::string {
            if (!ctx->sim.broadcaster || !ctx->sim.gameLoop || !ctx->sim.entityManager)
                return "flight: not available";
            if (args.empty())
                return "usage: flight list | create | add | order | disband";

            const std::string_view sub = args[0];

            if (sub == "list") {
                ctx->sim.gameLoop->enqueueSimCallback([ctx]() {
                    const auto& reg = ctx->sim.broadcaster->formations();
                    if (reg.size() == 0) {
                        printAdmin(*ctx, "[admin] no formations");
                        return;
                    }
                    reg.forEach([&](const fl::Formation& f) {
                        char m[320];
                        char cmdr[24];
                        if (f.commanderPeerId == fl::kNoPeer)
                            std::snprintf(cmdr, sizeof(cmdr), "game-master");
                        else
                            std::snprintf(cmdr, sizeof(cmdr), "peer %u", f.commanderPeerId);
                        std::snprintf(m, sizeof(m),
                                      "[admin] flight %u \"%s\"  anchor=%u  commander=%s  parent=%u  members=%zu",
                                      static_cast<unsigned>(f.id), f.callsign.c_str(), f.anchor.index, cmdr,
                                      static_cast<unsigned>(f.parent), f.members.size());
                        printAdmin(*ctx, m);
                        for (const fl::FormationMember& mem : f.members) {
                            char mm[192];
                            std::snprintf(mm, sizeof(mm), "[admin]   member entity=%u slot=%u %s%s", mem.id.index,
                                          mem.slotIndex, mem.isAi() ? "AI" : "HUMAN",
                                          mem.weaponsHold ? " [weapons hold]" : "");
                            printAdmin(*ctx, mm);
                        }
                    });
                });
                return "flight list: queued";
            }

            if (sub == "create") {
                if (args.size() < 2)
                    return "usage: flight create <anchorIdx> [--commander <peerId>] [--parent <id>] [--callsign "
                           "<name>]";
                uint32_t anchorIdx = 0;
                if (!parseU32(args[1], anchorIdx))
                    return "flight create: invalid anchor index";

                uint32_t commander = fl::kNoPeer; // no --commander = game-master-owned (NOT peer 0)
                uint32_t parent = 0;
                std::string callsign = "Flight";
                for (std::size_t i = 2; i + 1 < args.size(); ++i) {
                    if (args[i] == "--commander")
                        (void)parseU32(args[++i], commander);
                    else if (args[i] == "--parent")
                        (void)parseU32(args[++i], parent);
                    else if (args[i] == "--callsign")
                        callsign = std::string(args[++i]);
                }

                ctx->sim.gameLoop->enqueueSimCallback([ctx, anchorIdx, commander, parent, callsign]() {
                    fl::EntityId anchor;
                    ctx->sim.entityManager->forEach([&](const fl::EntityState& s) {
                        if (!anchor.valid() && s.id.index == anchorIdx && !s.dead)
                            anchor = s.id;
                    });
                    char m[192];
                    if (!anchor.valid()) {
                        std::snprintf(m, sizeof(m), "[admin] flight create: no live entity with index %u", anchorIdx);
                        printAdmin(*ctx, m);
                        return;
                    }
                    const fl::FormationId fid = ctx->sim.broadcaster->formations().create(
                        callsign, anchor, commander, static_cast<fl::FormationId>(parent));
                    if (fid == fl::kNoFormation) {
                        printAdmin(*ctx, "[admin] flight create: failed (unknown parent, or tree too deep)");
                        return;
                    }
                    if (commander == fl::kNoPeer)
                        std::snprintf(m, sizeof(m),
                                      "[admin] created flight %u \"%s\" anchored on entity %u (game-master commanded)",
                                      static_cast<unsigned>(fid), callsign.c_str(), anchorIdx);
                    else
                        std::snprintf(m, sizeof(m),
                                      "[admin] created flight %u \"%s\" anchored on entity %u (commander peer %u)",
                                      static_cast<unsigned>(fid), callsign.c_str(), anchorIdx, commander);
                    printAdmin(*ctx, m);
                });
                return "flight create: queued";
            }

            if (sub == "add") {
                if (args.size() < 3)
                    return "usage: flight add <flightId> <entityIdx> [slot]";
                uint32_t fid = 0, entIdx = 0, slot = 0;
                if (!parseU32(args[1], fid) || !parseU32(args[2], entIdx))
                    return "flight add: invalid id or entity index";
                if (args.size() >= 4)
                    (void)parseU32(args[3], slot);

                ctx->sim.gameLoop->enqueueSimCallback([ctx, fid, entIdx, slot]() {
                    fl::EntityId ent;
                    ctx->sim.entityManager->forEach([&](const fl::EntityState& s) {
                        if (!ent.valid() && s.id.index == entIdx && !s.dead)
                            ent = s.id;
                    });
                    char m[192];
                    if (!ent.valid()) {
                        std::snprintf(m, sizeof(m), "[admin] flight add: no live entity with index %u", entIdx);
                        printAdmin(*ctx, m);
                        return;
                    }

                    // Is a PERSON flying this? Resolve it against the live peer map rather than
                    // EntityState::ownerId, whose "0 = server/AI" convention collides with peer id 0
                    // — an ordinary player. Getting this wrong would mean the server retasks a live
                    // player's aircraft with an autopilot.
                    uint32_t ownerPeer = fl::kNoPeer;
                    ctx->sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                        if (pi.eid == ent)
                            ownerPeer = pi.peerId;
                    });

                    fl::FormationMember mem{};
                    mem.id = ent;
                    mem.peerId = ownerPeer; // a player's aircraft joins as a HUMAN member: orders are
                                            // relayed to them as radio calls, never applied to them
                    mem.slotIndex = slot;
                    if (!ctx->sim.broadcaster->formations().addMember(static_cast<fl::FormationId>(fid), mem)) {
                        std::snprintf(m, sizeof(m), "[admin] flight add: no such flight %u", fid);
                        printAdmin(*ctx, m);
                        return;
                    }
                    std::snprintf(m, sizeof(m), "[admin] added entity %u to flight %u as %s (slot %u)", entIdx, fid,
                                  ownerPeer == 0 ? "AI" : "HUMAN", slot);
                    printAdmin(*ctx, m);
                });
                return "flight add: queued";
            }

            if (sub == "order") {
                if (args.size() < 3)
                    return "usage: flight order <flightId> <command> [--member <entityIdx>] [--target "
                           "<entityIdx>] [--cascade]";
                uint32_t fid = 0;
                if (!parseU32(args[1], fid))
                    return "flight order: invalid flight id";
                const std::optional<fl::ai::WingmanCommand> cmd = fl::ai::parseWingmanCommand(args[2]);
                if (!cmd)
                    return "flight order: unknown command (attack_my_target, engage_bandits, rejoin, cover_me, "
                           "hold_fire, return_to_base)";

                uint32_t memberIdx = fl::kFlightAll;
                uint32_t targetIdx = fl::kFlightAll; // #861: --target designates for attack_my_target
                bool cascade = false;
                for (std::size_t i = 3; i < args.size(); ++i) {
                    if (args[i] == "--cascade")
                        cascade = true;
                    else if (args[i] == "--member" && i + 1 < args.size())
                        (void)parseU32(args[++i], memberIdx);
                    else if (args[i] == "--target" && i + 1 < args.size())
                        (void)parseU32(args[++i], targetIdx);
                }

                // attack_my_target normally needs a commander's boresight, which the console lacks. The
                // game master supplies the target explicitly via --target (an entity picked on the #861
                // map). Without one, refuse rather than silently degrading to "hold station".
                if (*cmd == fl::ai::WingmanCommand::AttackMyTarget && targetIdx == fl::kFlightAll)
                    return "flight order: attack_my_target needs a target -- pass --target <entityIdx> (the GM map "
                           "supplies it), or use `spawn --ai pursuit <idx>`";

                const auto ordinal = static_cast<uint8_t>(*cmd);
                ctx->sim.gameLoop->enqueueSimCallback([ctx, fid, ordinal, memberIdx, cascade, targetIdx]() {
                    // Resolve --target (an entity index picked on the GM map) to a live EntityId; an
                    // unresolvable index designates nothing (the order then refuses honestly).
                    fl::EntityId designated{};
                    if (targetIdx != fl::kFlightAll && ctx->sim.entityManager) {
                        if (const fl::EntityState* ts = ctx->sim.entityManager->getByIndex(targetIdx))
                            designated = ts->id;
                    }
                    const auto rep = ctx->sim.broadcaster->applyFlightOrder(static_cast<fl::FormationId>(fid), ordinal,
                                                                            memberIdx, cascade, designated);
                    char m[192];
                    std::snprintf(m, sizeof(m), "[admin] flight %u ordered %s: %d AI retasked, %d relayed to players%s",
                                  fid, std::string(fl::ai::kWingmanCommandNames[ordinal]).c_str(), rep.aiRetasked,
                                  rep.humansRelayed, rep.deadSkipped > 0 ? " (some members are dead)" : "");
                    printAdmin(*ctx, m);
                });
                return "flight order: queued";
            }

            if (sub == "disband") {
                if (args.size() < 2)
                    return "usage: flight disband <flightId>";
                uint32_t fid = 0;
                if (!parseU32(args[1], fid))
                    return "flight disband: invalid flight id";
                ctx->sim.gameLoop->enqueueSimCallback([ctx, fid]() {
                    // Children are re-parented, not destroyed: disbanding a package must not delete
                    // the flights inside it, and the aircraft keep flying whatever they were last told.
                    const bool ok = ctx->sim.broadcaster->formations().destroy(static_cast<fl::FormationId>(fid));
                    char m[128];
                    std::snprintf(m, sizeof(m), ok ? "[admin] disbanded flight %u" : "[admin] no such flight %u", fid);
                    printAdmin(*ctx, m);
                });
                return "flight disband: queued";
            }

            return "usage: flight list | create | add | order | disband";
        });

    registry.registerCommand("quit", "quit  -- shut down fl-server gracefully",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx->env.quitFlag)
                                     return "quit: not available";
                                 *ctx->env.quitFlag = 1;
                                 return "shutting down...";
                             });
}

} // namespace fl