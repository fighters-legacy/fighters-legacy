// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCommands.h"

#include "ai/AiControllerFactory.h"
#include "atc/AtcService.h" // atc_status/atc_scramble/atc_hold (#705)
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
#include <net/WorldBroadcaster.h>
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
static void printAdmin(const ServerCommandContext& ctx, const char* line) {
    std::printf("%s\n", line);
    if (ctx.rcon.shell)
        ctx.rcon.shell->print(line);
    std::fflush(stdout);
}

static bool parseU32(std::string_view s, uint32_t& out) {
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

// ---------------------------------------------------------------------------
// Local IP helpers (mirrors WorldBroadcaster.cpp — kept file-static)
// ---------------------------------------------------------------------------

static std::string normalizeIp(std::string_view raw) {
    std::string_view v = raw;
    if (!v.empty() && v.front() == '[') {
        v.remove_prefix(1);
        auto end = v.find(']');
        if (end != std::string_view::npos)
            v = v.substr(0, end);
    }
    std::string ip(v);
    if (ip.size() > 7 && ip.compare(0, 7, "::ffff:") == 0)
        ip.erase(0, 7);
    return ip;
}

// Extract the normalized IP from a full "ip:port" or "[ip]:port" string.
static std::string extractIp(std::string_view addrPort) {
    std::string_view v = addrPort;
    std::string_view ipv;
    if (!v.empty() && v.front() == '[') {
        v.remove_prefix(1);
        auto end = v.find(']');
        ipv = (end != std::string_view::npos) ? v.substr(0, end) : v;
    } else {
        auto colon = v.rfind(':');
        ipv = (colon != std::string_view::npos) ? v.substr(0, colon) : v;
    }
    return normalizeIp(ipv);
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

// Returns true when arg consists entirely of ASCII digits (treat as peerId).
static bool isNumeric(std::string_view arg) {
    if (arg.empty())
        return false;
    for (char c : arg)
        if (c < '0' || c > '9')
            return false;
    return true;
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

static std::string formatAuthSection(const char* label, const fl::AuthLockoutSummary& s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[admin] %s:", label);
    std::string out(buf);
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

void registerServerCommands(CommandRegistry& registry, ServerCommandContext ctx) {

    // help [command]
    registry.registerCommand("help", "help [command]  -- list all commands or show usage for one",
                             [&registry](std::span<std::string_view> args) -> std::string {
                                 if (!args.empty())
                                     return registry.helpFor(args[0]);
                                 return registry.helpText();
                             });

    // status
    registry.registerCommand(
        "status", "status  -- show server state (uptime, peer count, entity count, tick rate)",
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx.sim.broadcaster || !ctx.sim.entityManager)
                return "status: not available";
            using namespace std::chrono;
            auto uptimeSec = duration_cast<seconds>(steady_clock::now() - ctx.env.startTime).count();
            int peers = ctx.sim.broadcaster->getPeerCount();
            uint32_t entities = ctx.sim.entityManager->liveCount();
            const fl::TickBudget tb = ctx.sim.broadcaster->getTickBudget();
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "uptime: %llds  peers: %d  entities: %u  tick: %.1f Hz (%.2f/%.2f ms mean/p99)",
                          static_cast<long long>(uptimeSec), peers, entities, tb.tickHz, tb.total.mean, tb.total.p99);
            std::string out(buf);
            const fl::OverrunStatus ov = ctx.sim.broadcaster->getOverrunStatus();
            char ovbuf[96];
            std::snprintf(ovbuf, sizeof(ovbuf), "  load: %.0f%%  interest: %.0f%%%s", ov.loadFactor * 100.0,
                          ov.interestScale * 100.0, ov.degraded ? " [DEGRADED]" : "");
            out += ovbuf;
            auto ls = ctx.sim.broadcaster->getAuthLockoutSummary();
            if (ls.activeCount > 0) {
                char lbuf[96];
                std::snprintf(lbuf, sizeof(lbuf),
                              "\nadmin auth lockouts: %d active (use admin_auth_status for details)", ls.activeCount);
                out += lbuf;
            }
            return out;
        });

    // tickstats — per-phase server tick budget (integrate/ai/collision/serialize/total).
    registry.registerCommand(
        "tickstats", "tickstats  -- per-phase sim tick budget (ms: mean/p95/p99/max) + actual tick Hz",
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx.sim.broadcaster)
                return "tickstats: not available";
            const fl::TickBudget tb = ctx.sim.broadcaster->getTickBudget();
            if (tb.ticksSampled == 0)
                return "tickstats: no ticks sampled yet";
            std::string out;
            char hdr[160];
            std::snprintf(hdr, sizeof(hdr), "tick %.2f Hz  window %.1fs  samples %llu (total %llu)", tb.tickHz,
                          tb.windowSeconds, static_cast<unsigned long long>(tb.ticksSampled),
                          static_cast<unsigned long long>(tb.ticksTotal));
            out += hdr;
            const fl::OverrunStatus ov = ctx.sim.broadcaster->getOverrunStatus();
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
                             "fill/max, send rate, loss)",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                                     return "peers: not available";
                                 ctx.sim.gameLoop->enqueueSimCallback([ctx]() {
                                     int count = 0;
                                     ctx.sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                                         char m[384];
                                         std::snprintf(
                                             m, sizeof(m),
                                             "[admin] peer %u  %s  entity=%u/%u  delay=%ut (~%ums)"
                                             "  ewma=%.1ft  jitter=%.1ft  buf=%u/%u  rate=%.0fHz  loss=%.1f%%",
                                             pi.peerId, pi.addr.c_str(), pi.eid.index, pi.eid.generation, pi.delayTicks,
                                             (pi.delayTicks * 1000u + 30u) / 60u, pi.ewmaDelayTicks, pi.ewmaJitterTicks,
                                             pi.queueDepth, pi.bufferMaxDepth, static_cast<double>(pi.sendRateHz),
                                             static_cast<double>(pi.packetLoss) * 100.0);
                                         std::printf("%s\n", m);
                                         if (ctx.rcon.shell)
                                             ctx.rcon.shell->print(m);
                                         ++count;
                                     });
                                     if (count == 0) {
                                         std::printf("[admin] peers: no connected peers\n");
                                         if (ctx.rcon.shell)
                                             ctx.rcon.shell->print("[admin] peers: no connected peers");
                                     }
                                     std::fflush(stdout);
                                 });
                                 int count = ctx.sim.broadcaster->getPeerCount();
                                 char peerBuf[64];
                                 std::snprintf(peerBuf, sizeof(peerBuf), "%d peer(s) connected", count);
                                 return std::string(peerBuf);
                             });

    // kick <peerId|IP>
    registry.registerCommand("kick", "kick <peerId|IP>  -- disconnect a peer by ID or all peers from an IP address",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: kick <peerId|IP>";
                                 if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                                     return "kick: not available";
                                 std::string arg(args[0]);
                                 if (isNumeric(arg)) {
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return "kick: invalid peer ID";
                                     ctx.sim.gameLoop->enqueueSimCallback([ctx, peerId]() {
                                         ctx.sim.broadcaster->kickPeer(peerId);
                                         char m[64];
                                         std::snprintf(m, sizeof(m), "[admin] kicked peer %u", peerId);
                                         std::printf("%s\n", m);
                                         if (ctx.rcon.shell)
                                             ctx.rcon.shell->print(m);
                                         std::fflush(stdout);
                                     });
                                     char kickBuf[64];
                                     std::snprintf(kickBuf, sizeof(kickBuf), "kick: queued peer %u", peerId);
                                     return std::string(kickBuf);
                                 } else {
                                     std::string ip = normalizeIp(arg);
                                     ctx.sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                                         int kicked = 0;
                                         ctx.sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                                             if (extractIp(pi.addr) == ip) {
                                                 ctx.sim.broadcaster->kickPeer(pi.peerId);
                                                 ++kicked;
                                             }
                                         });
                                         char m[128];
                                         std::snprintf(m, sizeof(m), "[admin] kicked %d peer(s) from IP %s", kicked,
                                                       ip.c_str());
                                         std::printf("%s\n", m);
                                         if (ctx.rcon.shell)
                                             ctx.rcon.shell->print(m);
                                         std::fflush(stdout);
                                     });
                                     return "kick: queued peers from IP " + ip;
                                 }
                             });

    // set_role <peerId> <pilot|observer>  -- switch a peer between pilot and spectator without a reconnect (#857)
    registry.registerCommand(
        "set_role", "set_role <peerId> <pilot|observer>  -- switch a peer's role without a reconnect",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: set_role <peerId> <pilot|observer>";
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                return "set_role: not available";
            std::string idArg(args[0]);
            if (!isNumeric(idArg))
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
            ctx.sim.gameLoop->enqueueSimCallback([ctx, peerId, role]() {
                ctx.sim.broadcaster->setPeerRole(peerId, role);
                char m[80];
                std::snprintf(m, sizeof(m), "[admin] set peer %u role to %s", peerId,
                              role == fl::PeerRole::Observer ? "observer" : "pilot");
                std::printf("%s\n", m);
                if (ctx.rcon.shell)
                    ctx.rcon.shell->print(m);
                std::fflush(stdout);
            });
            char buf[80];
            std::snprintf(buf, sizeof(buf), "set_role: queued peer %u -> %s", peerId,
                          role == fl::PeerRole::Observer ? "observer" : "pilot");
            return std::string(buf);
        });

    // team <peerId> <factionIndex>  -- move a peer to a team (#522). Bypasses the balance guard (admin).
    registry.registerCommand(
        "team", "team <peerId> <factionIndex>  -- move a peer to a team (bypasses the balance guard)",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: team <peerId> <factionIndex>";
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                return "team: not available";
            std::string idArg(args[0]);
            std::string facArg(args[1]);
            if (!isNumeric(idArg) || !isNumeric(facArg))
                return "team: peerId and factionIndex must be numeric";
            uint32_t peerId = 0;
            unsigned faction = 0;
            if (auto [p, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), peerId); ec != std::errc{})
                return "team: invalid peer ID";
            if (auto [p, ec] = std::from_chars(facArg.data(), facArg.data() + facArg.size(), faction);
                ec != std::errc{} || faction > 0xFFFFu)
                return "team: invalid faction index";
            const uint16_t f = static_cast<uint16_t>(faction);
            ctx.sim.gameLoop->enqueueSimCallback([ctx, peerId, f]() {
                ctx.sim.broadcaster->setPeerFaction(peerId, f);
                char m[80];
                std::snprintf(m, sizeof(m), "[admin] moved peer %u to team %u", peerId, f);
                std::printf("%s\n", m);
                if (ctx.rcon.shell)
                    ctx.rcon.shell->print(m);
                std::fflush(stdout);
            });
            char buf[80];
            std::snprintf(buf, sizeof(buf), "team: queued peer %u -> team %u", peerId, f);
            return std::string(buf);
        });

    // seats <entityIdx>  -- inspect a crewed aircraft's seat roster/occupancy (#974)
    registry.registerCommand("seats", "seats <entityIdx>  -- show a crewed aircraft's seat roster and occupancy",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: seats <entityIdx>";
                                 if (!ctx.sim.broadcaster)
                                     return "seats: not available";
                                 std::string idArg(args[0]);
                                 if (!isNumeric(idArg))
                                     return "seats: invalid entity index";
                                 uint32_t idx = 0;
                                 if (auto [ptr, ec] = std::from_chars(idArg.data(), idArg.data() + idArg.size(), idx);
                                     ec != std::errc{})
                                     return "seats: invalid entity index";
                                 return ctx.sim.broadcaster->crewRosterText(idx);
                             });

    // set_seat <entityIdx> <seat> <peerId|bot|empty>  -- force a non-fly seat's occupancy (#974)
    registry.registerCommand(
        "set_seat", "set_seat <entityIdx> <seat> <peerId|bot|empty>  -- force a non-fly seat's occupancy",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 3)
                return "usage: set_seat <entityIdx> <seat> <peerId|bot|empty>";
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                return "set_seat: not available";
            std::string idArg(args[0]), seatArg(args[1]);
            if (!isNumeric(idArg) || !isNumeric(seatArg))
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
                if (!isNumeric(peerArg))
                    return "set_seat: third arg must be a peerId, 'bot', or 'empty'";
                if (auto [p, ec] = std::from_chars(peerArg.data(), peerArg.data() + peerArg.size(), peerId);
                    ec != std::errc{})
                    return "set_seat: invalid peerId";
                occ = fl::SeatOccupancy::Human;
            }
            const auto seat8 = static_cast<uint8_t>(seat);
            ctx.sim.gameLoop->enqueueSimCallback([ctx, entityIdx, seat8, occ, peerId]() {
                const std::string err = ctx.sim.broadcaster->adminSetSeat(entityIdx, seat8, occ, peerId);
                char m[128];
                if (err.empty())
                    std::snprintf(m, sizeof(m), "[admin] set entity %u seat %u -> %s", entityIdx, seat8,
                                  occ == fl::SeatOccupancy::Human ? "human"
                                  : occ == fl::SeatOccupancy::Bot ? "bot"
                                                                  : "empty");
                else
                    std::snprintf(m, sizeof(m), "[admin] %s", err.c_str());
                std::printf("%s\n", m);
                if (ctx.rcon.shell)
                    ctx.rcon.shell->print(m);
                std::fflush(stdout);
            });
            char buf[96];
            std::snprintf(buf, sizeof(buf), "set_seat: queued entity %u seat %u", entityIdx, seat8);
            return std::string(buf);
        });

    // ban <peerId|IP>
    registry.registerCommand("ban", "ban <peerId|IP>  -- add IP to in-memory ban list and kick matching peers",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: ban <peerId|IP>";
                                 if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                                     return "ban: not available";
                                 std::string arg(args[0]);
                                 if (isNumeric(arg)) {
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return "ban: invalid peer ID";
                                     ctx.sim.gameLoop->enqueueSimCallback([ctx, peerId]() {
                                         std::string foundIp;
                                         ctx.sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                                             if (pi.peerId == peerId)
                                                 foundIp = extractIp(pi.addr);
                                         });
                                         char m[128];
                                         if (foundIp.empty()) {
                                             std::snprintf(m, sizeof(m), "[admin] ban: peer %u not found", peerId);
                                         } else {
                                             ctx.sim.broadcaster->banAddress(foundIp);
                                             if (ctx.bans.saveBanlist)
                                                 ctx.bans.saveBanlist(ctx.sim.broadcaster->getBannedAddresses());
                                             std::snprintf(m, sizeof(m), "[admin] banned IP %s (peer %u)",
                                                           foundIp.c_str(), peerId);
                                         }
                                         std::printf("%s\n", m);
                                         if (ctx.rcon.shell)
                                             ctx.rcon.shell->print(m);
                                         std::fflush(stdout);
                                     });
                                     char banBuf[64];
                                     std::snprintf(banBuf, sizeof(banBuf), "ban: queued for peer %u", peerId);
                                     return std::string(banBuf);
                                 } else {
                                     std::string ip = normalizeIp(arg);
                                     ctx.sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                                         ctx.sim.broadcaster->banAddress(ip);
                                         if (ctx.bans.saveBanlist)
                                             ctx.bans.saveBanlist(ctx.sim.broadcaster->getBannedAddresses());
                                         char m[128];
                                         std::snprintf(m, sizeof(m), "[admin] banned IP %s", ip.c_str());
                                         std::printf("%s\n", m);
                                         if (ctx.rcon.shell)
                                             ctx.rcon.shell->print(m);
                                         std::fflush(stdout);
                                     });
                                     return "ban: banning IP " + ip;
                                 }
                             });

    // unban <IP>
    registry.registerCommand("unban", "unban <IP>  -- remove an IP from the in-memory ban list",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: unban <IP>";
                                 if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                                     return "unban: not available";
                                 std::string ip = normalizeIp(args[0]);
                                 ctx.sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                                     ctx.sim.broadcaster->unbanAddress(ip);
                                     if (ctx.bans.saveBanlist)
                                         ctx.bans.saveBanlist(ctx.sim.broadcaster->getBannedAddresses());
                                     char m[128];
                                     std::snprintf(m, sizeof(m), "[admin] unbanned IP %s", ip.c_str());
                                     std::printf("%s\n", m);
                                     if (ctx.rcon.shell)
                                         ctx.rcon.shell->print(m);
                                     std::fflush(stdout);
                                 });
                                 return "unban: unbanning IP " + ip;
                             });

    // admin_unlock <IP>
    registry.registerCommand(
        "admin_unlock", "admin_unlock <IP>  -- clear admin and RCON auth lockouts for an IP address",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: admin_unlock <IP>";
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                return "admin_unlock: not available";
            std::string ip = normalizeIp(args[0]);
            ctx.sim.gameLoop->enqueueSimCallback([ctx, ip]() {
                bool adminWasLocked = ctx.sim.broadcaster->unlockAdminAuth(ip);
                bool rconWasLocked = ctx.rcon.clearRconLockout ? ctx.rcon.clearRconLockout(ip) : false;
                bool anyWasLocked = adminWasLocked || rconWasLocked;
                char m[128];
                if (anyWasLocked) {
                    if (ctx.rcon.clearRconLockout)
                        std::snprintf(m, sizeof(m), "[admin] unlocked %s (admin + RCON)", ip.c_str());
                    else
                        std::snprintf(m, sizeof(m), "[admin] unlocked %s", ip.c_str());
                } else {
                    std::snprintf(m, sizeof(m), "[admin] admin_unlock: %s was not locked", ip.c_str());
                }
                std::printf("%s\n", m);
                if (ctx.rcon.shell)
                    ctx.rcon.shell->print(m);
                std::fflush(stdout);
            });
            return "admin_unlock: queued for " + ip;
        });

    // admin_auth_status
    registry.registerCommand("admin_auth_status",
                             "admin_auth_status  -- show per-IP auth lockout state for admin and RCON channels",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.sim.broadcaster)
                                     return "admin_auth_status: not available";
                                 auto adminS = ctx.sim.broadcaster->getAuthLockoutSummary();
                                 bool hasRcon = static_cast<bool>(ctx.rcon.getRconAuthSummary);
                                 auto rconS = hasRcon ? ctx.rcon.getRconAuthSummary() : fl::AuthLockoutSummary{};

                                 std::string detail = formatAuthSection("MsgAdminCommand channel", adminS);
                                 if (hasRcon) {
                                     detail += "\n\n";
                                     detail += formatAuthSection("RCON channel", rconS);
                                 }
                                 std::printf("%s\n", detail.c_str());
                                 std::fflush(stdout);
                                 return detail;
                             });

    // set_weather <preset>
    registry.registerCommand(
        "set_weather", "set_weather <clear|partly_cloudy|overcast|rain|storm|snow|blizzard>  -- change weather preset",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: set_weather <clear|partly_cloudy|overcast|rain|storm|snow|blizzard>";
            if (!ctx.sim.weatherController || !ctx.sim.gameLoop)
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
            ctx.sim.gameLoop->enqueueSimCallback([ctx, preset]() { ctx.sim.weatherController->setPreset(preset); });
            return std::string("set_weather: ") + std::string(args[0]);
        });

    // set_time <hours>
    registry.registerCommand("set_time", "set_time <0-24>  -- set in-game time of day (hours, float)",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: set_time <0-24>";
                                 // Validate argument before context check so parse/range errors are always reported.
                                 // Use strtof rather than from_chars<float>: the float overload is deleted on
                                 // Apple Clang before macOS 13.3 (Xcode 14.3).
                                 std::string timeStr(args[0]);
                                 char* timeEnd = nullptr;
                                 errno = 0;
                                 float hours = std::strtof(timeStr.c_str(), &timeEnd);
                                 if (timeEnd == timeStr.c_str() || *timeEnd != '\0' || errno == ERANGE)
                                     return "set_time: invalid value";
                                 if (hours < 0.f || hours > 24.f)
                                     return "set_time: value must be in [0, 24]";
                                 if (!ctx.sim.weatherController || !ctx.sim.gameLoop)
                                     return "set_time: not available";
                                 ctx.sim.gameLoop->enqueueSimCallback(
                                     [ctx, hours]() { ctx.sim.weatherController->setTimeOfDay(hours); });
                                 char buf[64];
                                 std::snprintf(buf, sizeof(buf), "set_time: %.2f", hours);
                                 return buf;
                             });

    // ── Air-traffic control (#705) ───────────────────────────────────────────
    registry.registerCommand("atc_status", "atc_status [airport]  -- show ATC facility queues and runway occupancy",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (!ctx.sim.atc)
                                     return "atc_status: ATC not available";
                                 const std::string filter = args.empty() ? std::string{} : std::string(args[0]);
                                 return ctx.sim.atc->statusText(filter);
                             });

    registry.registerCommand(
        "atc_scramble", "atc_scramble <airport> <type> [count]  -- launch AI departures from a named airport",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 2)
                return "usage: atc_scramble <airport> <type> [count]";
            if (!ctx.sim.atc || !ctx.sim.gameLoop)
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
            ctx.sim.gameLoop->enqueueSimCallback([ctx, airport, type, count]() {
                const bool ok = ctx.sim.atc->scramble(airport, type, count);
                printAdmin(ctx, ok ? "[atc] scramble launched" : "[atc] scramble failed (unknown airport?)");
            });
            char buf[96];
            std::snprintf(buf, sizeof(buf), "atc_scramble: queued %d from %s", count, airport.c_str());
            return std::string(buf);
        });

    registry.registerCommand("atc_hold", "atc_hold <airport> <on|off>  -- freeze or release departures at an airport",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.size() < 2)
                                     return "usage: atc_hold <airport> <on|off>";
                                 if (!ctx.sim.atc || !ctx.sim.gameLoop)
                                     return "atc_hold: ATC not available";
                                 bool hold;
                                 if (args[1] == "on")
                                     hold = true;
                                 else if (args[1] == "off")
                                     hold = false;
                                 else
                                     return "atc_hold: second argument must be on|off";
                                 std::string airport(args[0]);
                                 ctx.sim.gameLoop->enqueueSimCallback(
                                     [ctx, airport, hold]() { ctx.sim.atc->holdDepartures(airport, hold); });
                                 return std::string("atc_hold: ") + (hold ? "holding " : "releasing ") + airport;
                             });

    // spawn <type> <x> <y> <z> [--faction <n>] [--ai <behavior> [behavior-args...]]
    registry.registerCommand(
        "spawn",
        "spawn <type> <x> <y> <z> [--faction <n>] [--ai <behavior> [args...]]  -- spawn entity with optional "
        "faction and AI controller",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 4)
                return "usage: spawn <type> <x> <y> <z> [--faction <n>] [--ai <behavior> [args...]]";
            if (!ctx.sim.entityManager || !ctx.sim.gameLoop)
                return "spawn: not available";
            std::string typeId(args[0]);
            double x = 0, y = 0, z = 0;
            auto parseD = [](std::string_view s, double& out) {
                std::string tmp(s);
                char* end = nullptr;
                errno = 0;
                float f = std::strtof(tmp.c_str(), &end);
                if (end == tmp.c_str() || *end != '\0' || errno == ERANGE)
                    return false;
                out = static_cast<double>(f);
                return true;
            };
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
            // ctx.env.loadAIScript reads from a pre-loaded read-only cache — safe from any thread.
            std::string luaScriptSrc;
            std::string luaScriptRoot;
            std::string effectiveBehavior = behavior; // may change to "lua" from aiScriptAsset auto-detect

            if (behavior == "lua") {
                if (!ctx.env.loadAIScript)
                    return "spawn: --ai lua: Lua AI scripting not available";
                std::string scriptName = behaviorArgStrings.empty() ? "" : behaviorArgStrings[0];
                if (scriptName.empty())
                    return "spawn: --ai lua requires a script name";
                auto [src, root] = ctx.env.loadAIScript(scriptName);
                if (src.empty()) {
                    char em[128];
                    std::snprintf(em, sizeof(em), "spawn: --ai lua: script '%s' not found", scriptName.c_str());
                    return std::string(em);
                }
                luaScriptSrc = std::move(src);
                luaScriptRoot = std::move(root);
            } else if (behavior.empty() && ctx.sim.typeRegistry && ctx.env.loadAIScript) {
                // Auto-detect: check if the entity type has a default AI script.
                const fl::EntityDef* def = ctx.sim.typeRegistry->findById(typeId.c_str());
                if (def && !def->aiScriptAsset.empty()) {
                    auto [src, root] = ctx.env.loadAIScript(def->aiScriptAsset);
                    if (!src.empty()) {
                        luaScriptSrc = std::move(src);
                        luaScriptRoot = std::move(root);
                        effectiveBehavior = "lua:" + def->aiScriptAsset;
                    }
                }
            }

            ctx.sim.gameLoop->enqueueSimCallback(
                [ctx, typeId, x, y, z, factionIndex, behavior, behaviorArgStrings, luaScriptSrc, luaScriptRoot]() {
                    fl::EntityTransform t{};
                    t.pos[0] = x;
                    t.pos[1] = y;
                    t.pos[2] = z;
                    fl::EntityId id = ctx.sim.entityManager->spawn(typeId.c_str(), t);
                    char m[160];
                    if (id.valid()) {
                        // Faction is server-assigned at spawn time (#465); 0 = neutral (default).
                        if (factionIndex != 0) {
                            if (fl::EntityState* s = ctx.sim.entityManager->get(id))
                                s->factionIndex = factionIndex;
                        }
                        std::snprintf(m, sizeof(m), "[admin] spawned %s entity=%u/%u", typeId.c_str(), id.index,
                                      id.generation);
                        std::printf("%s\n", m);
                        if (ctx.rcon.shell)
                            ctx.rcon.shell->print(m);

                        if (ctx.sim.broadcaster) {
                            std::unique_ptr<fl::IEntityController> ctrl;

                            if (!luaScriptSrc.empty()) {
                                // Lua AI controller — constructed on sim thread; the world.* seam (#413)
                                // lets an admin-spawned script reach spawn/faction/mission/music too.
                                auto luaCtrl = std::make_unique<LuaController>(
                                    luaScriptSrc, luaScriptRoot, ctx.sim.entityManager, ctx.sim.worldApi, ctx.sim.atc);
                                if (luaCtrl->isValid()) {
                                    ctrl = std::move(luaCtrl);
                                } else {
                                    char em[192];
                                    std::snprintf(em, sizeof(em), "[admin] spawn: Lua script error: %s",
                                                  luaCtrl->lastError().c_str());
                                    std::printf("%s\n", em);
                                    if (ctx.rcon.shell)
                                        ctx.rcon.shell->print(em);
                                }
                            } else if (!behavior.empty() && behavior != "lua") {
                                // C++ AI controller via factory.
                                std::vector<std::string_view> argViews;
                                argViews.reserve(behaviorArgStrings.size());
                                for (const auto& s : behaviorArgStrings)
                                    argViews.push_back(s);
                                ctrl = fl::ai::createController(behavior, std::span<std::string_view>(argViews),
                                                                ctx.sim.entityManager);
                                if (!ctrl) {
                                    char wm[128];
                                    std::snprintf(wm, sizeof(wm), "[admin] spawn: unknown AI behavior '%s' or bad args",
                                                  behavior.c_str());
                                    std::printf("%s\n", wm);
                                    if (ctx.rcon.shell)
                                        ctx.rcon.shell->print(wm);
                                }
                            }

                            if (ctrl) {
                                ctx.sim.broadcaster->registerController(id, std::move(ctrl));
                                char am[128];
                                std::snprintf(am, sizeof(am), "[admin] attached AI '%s' to entity=%u",
                                              behavior.empty() ? "lua(auto)" : behavior.c_str(), id.index);
                                std::printf("%s\n", am);
                                if (ctx.rcon.shell)
                                    ctx.rcon.shell->print(am);
                            }
                        }
                    } else {
                        std::snprintf(m, sizeof(m), "[admin] spawn: type '%s' unknown or cap reached", typeId.c_str());
                        std::printf("%s\n", m);
                        if (ctx.rcon.shell)
                            ctx.rcon.shell->print(m);
                    }
                    std::fflush(stdout);
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
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: kill <idx>";
            if (!ctx.sim.entityManager || !ctx.sim.gameLoop)
                return "kill: not available";
            uint32_t targetIdx = 0;
            auto [ptr, ec] = std::from_chars(args[0].data(), args[0].data() + args[0].size(), targetIdx);
            if (ec != std::errc{})
                return "kill: invalid index";
            ctx.sim.gameLoop->enqueueSimCallback([ctx, targetIdx]() {
                fl::EntityId killId;
                ctx.sim.entityManager->forEach([&](const fl::EntityState& state) {
                    if (!killId.valid() && state.id.index == targetIdx)
                        killId = state.id;
                });
                char m[128];
                if (killId.valid()) {
                    ctx.sim.entityManager->kill(killId);
                    std::snprintf(m, sizeof(m), "[admin] killed entity %u/%u", killId.index, killId.generation);
                } else {
                    std::snprintf(m, sizeof(m), "[admin] kill: no live entity with index %u", targetIdx);
                }
                std::printf("%s\n", m);
                if (ctx.rcon.shell)
                    ctx.rcon.shell->print(m);
                std::fflush(stdout);
            });
            char killBuf[64];
            std::snprintf(killBuf, sizeof(killBuf), "kill: queued index %u", targetIdx);
            return std::string(killBuf);
        });

    // tp <idx> <x> <y> <z>
    registry.registerCommand(
        "tp", "tp <idx> <x> <y> <z>  -- teleport entity to world position",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 4)
                return "usage: tp <idx> <x> <y> <z>";
            if (!ctx.sim.entityManager || !ctx.sim.gameLoop)
                return "tp: not available";
            uint32_t targetIdx = 0;
            auto [ptr, ec] = std::from_chars(args[0].data(), args[0].data() + args[0].size(), targetIdx);
            if (ec != std::errc{})
                return "tp: invalid entity index";
            // Parse coordinates with strtod (from_chars for double not on Apple Clang).
            auto parseCoord = [](std::string_view sv, double& out) -> bool {
                if (sv.empty())
                    return false;
                std::string s(sv); // null-terminated for strtod
                char* end = nullptr;
                out = std::strtod(s.c_str(), &end);
                return end == s.c_str() + sv.size() && end != s.c_str();
            };
            double x{}, y{}, z{};
            if (!parseCoord(args[1], x) || !parseCoord(args[2], y) || !parseCoord(args[3], z))
                return "tp: invalid coordinates";
            ctx.sim.gameLoop->enqueueSimCallback([ctx, targetIdx, x, y, z]() {
                ctx.sim.entityManager->forEach([&](fl::EntityState& state) {
                    if (state.id.index == targetIdx) {
                        state.transform.pos[0] = x;
                        state.transform.pos[1] = y;
                        state.transform.pos[2] = z;
                        char m[128];
                        std::snprintf(m, sizeof(m), "[admin] teleported entity %u/%u to X:%+.1f Y:%+.1f Z:%+.1f",
                                      state.id.index, state.id.generation, static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(z));
                        std::printf("%s\n", m);
                        if (ctx.rcon.shell)
                            ctx.rcon.shell->print(m);
                        std::fflush(stdout);
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
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 5)
                return "usage: detonate <x> <y> <z> <blast_radius_m> <damage> [--nuclear]";
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                return "detonate: not available";
            auto parseNum = [](std::string_view sv, double& out) -> bool {
                if (sv.empty())
                    return false;
                std::string s(sv); // null-terminated for strtod (no float from_chars on Apple Clang)
                char* end = nullptr;
                out = std::strtod(s.c_str(), &end);
                return end == s.c_str() + sv.size() && end != s.c_str();
            };
            double x{}, y{}, z{}, radius{}, damage{};
            if (!parseNum(args[0], x) || !parseNum(args[1], y) || !parseNum(args[2], z) || !parseNum(args[3], radius) ||
                !parseNum(args[4], damage))
                return "detonate: invalid arguments";
            if (radius <= 0.0 || damage <= 0.0)
                return "detonate: blast_radius_m and damage must be > 0";
            const bool nuclear = args.size() > 5 && args[5] == "--nuclear";

            ctx.sim.gameLoop->enqueueSimCallback([ctx, x, y, z, radius, damage, nuclear]() {
                fl::BlastSpec blast;
                blast.radiusM = static_cast<float>(radius);
                blast.damage = static_cast<float>(damage);
                blast.nuclear = nuclear;
                const double pos[3] = {x, y, z};
                const fl::WarheadResult r = ctx.sim.broadcaster->applyWarheadAt(pos, blast, fl::EntityId::null());
                char m[160];
                std::snprintf(m, sizeof(m),
                              "[admin] detonated %.0f dmg / %.0f m%s at X:%+.1f Y:%+.1f Z:%+.1f — "
                              "%d damaged, %d EMPed",
                              damage, radius, nuclear ? " (nuclear)" : "", x, y, z, r.damaged, r.emped);
                std::printf("%s\n", m);
                if (ctx.rcon.shell)
                    ctx.rcon.shell->print(m);
                std::fflush(stdout);
            });
            char buf[96];
            std::snprintf(buf, sizeof(buf), "detonate: queued %.0f dmg / %.0f m%s", damage, radius,
                          nuclear ? " (nuclear)" : "");
            return std::string(buf);
        });

    // reload_config
    registry.registerCommand(
        "reload_config",
        "reload_config  -- re-read server.toml and apply: name (beacon), motd, motd_display_s,"
        " draw_distance_km, snapshot_budget_bytes, jitter_buffer_depth,"
        " jitter_buffer_adapt_window, jitter_buffer_hysteresis, jitter_buffer_jitter_multiplier,"
        " congestion_enabled, congestion_min_send_hz, congestion_loss_threshold,"
        " congestion_budget_floor_bytes, overrun_governor_enabled, overrun_high_watermark,"
        " overrun_low_watermark, overrun_min_snapshot_hz, overrun_max_ai_stride,"
        " overrun_budget_floor_bytes, overrun_min_interest_fraction, compress_snapshots,"
        " sensor_check_hz, gameplay.friendly_fire, gameplay.crash_damage, ai.difficulty (other"
        " fields, incl. max_catchup_ticks and gns_nagle_time_us, require restart)",
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx.env.configPath || ctx.env.configPath->empty())
                return "reload_config: not available";
            std::ifstream f(*ctx.env.configPath);
            if (!f)
                return "reload_config: cannot open " + *ctx.env.configPath;
            std::ostringstream ss;
            ss << f.rdbuf();
            ServerConfig newCfg = parseServerConfig(ss.str(), ctx.env.logger);
            if (ctx.env.beacon)
                ctx.env.beacon->setName(newCfg.name);
            if (ctx.sim.broadcaster && ctx.sim.gameLoop) {
                auto newMotd = newCfg.motd;
                auto newMotdDisplayS = newCfg.motdDisplayS;
                auto newDraw = static_cast<float>(newCfg.drawDistanceKm);
                auto newSnapshotBudget = newCfg.snapshotBudgetBytes;
                auto newJitterDepth = newCfg.jitterBufferDepth;
                auto newAdaptWindow = newCfg.jitterAdaptWindow;
                auto newHysteresis = newCfg.jitterHysteresis;
                auto newMultiplier = newCfg.jitterMultiplier;
                auto newCongestion =
                    fl::makeCongestionParams(newCfg.congestionEnabled, newCfg.congestionMinSendHz,
                                             newCfg.congestionLossThreshold, newCfg.congestionBudgetFloorBytes);
                auto newGovernor = fl::makeTickGovernorParams(
                    newCfg.overrunGovernorEnabled, newCfg.overrunHighWatermark, newCfg.overrunLowWatermark,
                    newCfg.overrunMinSnapshotHz, newCfg.overrunMaxAiStride, newCfg.overrunBudgetFloorBytes,
                    newCfg.overrunMinInterestFraction);
                auto newCompress = newCfg.network.compressSnapshots;
                auto newSensorHz = static_cast<float>(newCfg.sensorCheckHz);
                auto newRules = fl::DamageRules{newCfg.friendlyFire, newCfg.crashDamage};
                // Resolve the preset OFF the sim thread (it may read data/difficulty.toml through the
                // AssetManager); only the resulting POD crosses into the callback. Null resolver ⇒
                // nullopt ⇒ the running scaling is left alone rather than reset to a default.
                std::optional<fl::AiScaling> newAiScaling;
                if (ctx.env.resolveAiScaling)
                    newAiScaling = ctx.env.resolveAiScaling(newCfg.aiDifficulty);
                ctx.sim.gameLoop->enqueueSimCallback([ctx, newMotd, newMotdDisplayS, newDraw, newSnapshotBudget,
                                                      newJitterDepth, newAdaptWindow, newHysteresis, newMultiplier,
                                                      newCongestion, newGovernor, newCompress, newSensorHz, newRules,
                                                      newAiScaling]() mutable {
                    ctx.sim.broadcaster->setMotd(std::move(newMotd));
                    ctx.sim.broadcaster->setMotdDisplaySeconds(newMotdDisplayS);
                    ctx.sim.broadcaster->setDrawDistance(newDraw);
                    ctx.sim.broadcaster->setSnapshotBudget(newSnapshotBudget);
                    ctx.sim.broadcaster->setSnapshotCompression(newCompress);
                    ctx.sim.broadcaster->setJitterBufferDepth(newJitterDepth);
                    ctx.sim.broadcaster->setJitterAdaptWindow(newAdaptWindow);
                    ctx.sim.broadcaster->setJitterHysteresis(newHysteresis);
                    ctx.sim.broadcaster->setJitterMultiplier(newMultiplier);
                    ctx.sim.broadcaster->setCongestionParams(newCongestion);
                    ctx.sim.broadcaster->setGovernorParams(newGovernor);
                    ctx.sim.broadcaster->setSensorCheckHz(newSensorHz);
                    ctx.sim.broadcaster->setDamageRules(newRules);
                    if (newAiScaling)
                        ctx.sim.broadcaster->setAiScaling(*newAiScaling);
                });
            }
            return "reload_config: name=\"" + newCfg.name + "\"  motd=\"" + newCfg.motd +
                   "\"  motd_display_s=" + std::to_string(newCfg.motdDisplayS) +
                   "  draw_distance_km=" + std::to_string(newCfg.drawDistanceKm) +
                   "  snapshot_budget_bytes=" + std::to_string(newCfg.snapshotBudgetBytes) +
                   "  jitter_buffer_depth=" + std::to_string(newCfg.jitterBufferDepth) +
                   "  jitter_buffer_adapt_window=" + std::to_string(newCfg.jitterAdaptWindow) +
                   "  jitter_buffer_hysteresis=" + std::to_string(newCfg.jitterHysteresis) +
                   "  jitter_buffer_jitter_multiplier=" + std::to_string(newCfg.jitterMultiplier) +
                   "  sensor_check_hz=" + std::to_string(newCfg.sensorCheckHz) + "  ai.difficulty=\"" +
                   newCfg.aiDifficulty + "\"" + "  (other fields require restart)";
        });

    // reload_banlist
    registry.registerCommand("reload_banlist",
                             "reload_banlist  -- reload ban list from security.banlist_path in server.toml",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.bans.banlistPath || ctx.bans.banlistPath->empty())
                                     return "reload_banlist: not available (security.banlist_path not configured)";
                                 if (!ctx.sim.broadcaster || !ctx.sim.gameLoop || !ctx.bans.loadBanlist)
                                     return "reload_banlist: not available";
                                 auto banned = ctx.bans.loadBanlist();
                                 auto count = banned.size();
                                 ctx.sim.gameLoop->enqueueSimCallback([ctx, b = std::move(banned)]() mutable {
                                     ctx.sim.broadcaster->setBannedAddresses(std::move(b));
                                     std::printf("[admin] reload_banlist: applied\n");
                                     if (ctx.rcon.shell)
                                         ctx.rcon.shell->print("[admin] reload_banlist: applied");
                                     std::fflush(stdout);
                                 });
                                 char buf[128];
                                 std::snprintf(buf, sizeof(buf), "reload_banlist: loading %zu IPs from %s", count,
                                               ctx.bans.banlistPath->c_str());
                                 return buf;
                             });

    // reload_allowlist
    registry.registerCommand("reload_allowlist",
                             "reload_allowlist  -- reload allowlist from security.allowlist_path in server.toml",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.bans.allowlistPath || ctx.bans.allowlistPath->empty())
                                     return "reload_allowlist: not available (security.allowlist_path not configured)";
                                 if (!ctx.sim.broadcaster || !ctx.sim.gameLoop || !ctx.bans.loadAllowlist)
                                     return "reload_allowlist: not available";
                                 auto allowed = ctx.bans.loadAllowlist();
                                 auto count = allowed.size();
                                 ctx.sim.gameLoop->enqueueSimCallback([ctx, a = std::move(allowed)]() mutable {
                                     ctx.sim.broadcaster->setAllowedAddresses(std::move(a));
                                     std::printf("[admin] reload_allowlist: applied\n");
                                     if (ctx.rcon.shell)
                                         ctx.rcon.shell->print("[admin] reload_allowlist: applied");
                                     std::fflush(stdout);
                                 });
                                 char buf[128];
                                 std::snprintf(buf, sizeof(buf), "reload_allowlist: loading %zu IPs from %s", count,
                                               ctx.bans.allowlistPath->c_str());
                                 return buf;
                             });

    // trace_start / trace_stop -- server-side input tracing (#560). Toggles recording of every
    // peer's accepted MsgClientInput to per-peer FLIT traces for bot_swarm `--pattern trace:` replay.
    registry.registerCommand(
        "trace_start", "trace_start [dir]  -- start recording peer inputs to FLIT traces (default: configured dir)",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                return "trace_start: not available";
            std::string dir;
            if (!args.empty())
                dir = std::string(args[0]);
            else if (!ctx.env.traceDir.empty())
                dir = ctx.env.traceDir;
            else
                dir = "traces";
            ctx.sim.gameLoop->enqueueSimCallback([ctx, dir]() {
                ctx.sim.broadcaster->setInputTraceDir(dir);
                char m[256];
                std::snprintf(m, sizeof(m), "[admin] trace_start: recording peer inputs to %s", dir.c_str());
                std::printf("%s\n", m);
                if (ctx.rcon.shell)
                    ctx.rcon.shell->print(m);
                std::fflush(stdout);
            });
            char buf[256];
            std::snprintf(buf, sizeof(buf), "trace_start: recording peer inputs to %s", dir.c_str());
            return buf;
        });

    registry.registerCommand("trace_stop", "trace_stop  -- stop input tracing and close all trace files",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                                     return "trace_stop: not available";
                                 ctx.sim.gameLoop->enqueueSimCallback([ctx]() {
                                     ctx.sim.broadcaster->setInputTraceDir("");
                                     std::printf("[admin] trace_stop: input tracing stopped\n");
                                     if (ctx.rcon.shell)
                                         ctx.rcon.shell->print("[admin] trace_stop: input tracing stopped");
                                     std::fflush(stdout);
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
        [ctx](std::span<std::string_view> args) -> std::string {
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
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
                ctx.sim.gameLoop->enqueueSimCallback([ctx]() {
                    char m[128];
                    if (ctx.sim.broadcaster->isShuttingDown()) {
                        uint32_t secs = ctx.sim.broadcaster->secondsUntilShutdown();
                        std::snprintf(m, sizeof(m), "[admin] shutdown scheduled in %u seconds", secs);
                    } else {
                        std::snprintf(m, sizeof(m), "[admin] no shutdown scheduled");
                    }
                    std::printf("%s\n", m);
                    if (ctx.rcon.shell)
                        ctx.rcon.shell->print(m);
                    std::fflush(stdout);
                });
                return "shutdown: status queued";
            }

            // --cancel
            if (flagCancel) {
                ctx.sim.gameLoop->enqueueSimCallback([ctx]() { ctx.sim.broadcaster->cancelShutdown(); });
                return "shutdown: cancelled";
            }

            // --delay (push back existing shutdown)
            if (flagDelay) {
                uint32_t extra = *flagDelay;
                ctx.sim.gameLoop->enqueueSimCallback([ctx, extra]() {
                    char m[128];
                    if (!ctx.sim.broadcaster->extendShutdown(extra))
                        std::snprintf(m, sizeof(m), "[admin] shutdown --delay: no active shutdown");
                    else
                        std::snprintf(m, sizeof(m), "[admin] shutdown delayed by %u seconds", extra);
                    std::printf("%s\n", m);
                    if (ctx.rcon.shell)
                        ctx.rcon.shell->print(m);
                    std::fflush(stdout);
                });
                return "shutdown: extension queued";
            }

            // --now or --in: confirmation gate.
            if (ctx.shutdown.requireConfirm && !flagForce) {
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
            if (flagIn && *flagIn < ctx.shutdown.minDelayS) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "shutdown: delay must be at least %u seconds (config min_shutdown_delay_s)",
                              ctx.shutdown.minDelayS);
                return buf;
            }

            // Schedule shutdown.
            uint32_t delaySecs = flagNow ? 0u : *flagIn;
            uint32_t intervalSecs = flagInterval.value_or(ctx.shutdown.warningIntervalS);
            ctx.sim.gameLoop->enqueueSimCallback([ctx, delaySecs, intervalSecs, flagReason]() {
                ctx.sim.broadcaster->initiateShutdown(delaySecs, intervalSecs, flagReason);
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
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.sim.gameLoop)
                                     return "pause: game loop not available";
                                 ctx.sim.gameLoop->setRate(TimeRate::Paused);
                                 if (ctx.rcon.shell)
                                     ctx.rcon.shell->print("simulation paused");
                                 return "simulation paused";
                             });

    registry.registerCommand("resume", "resume  -- resume the simulation at normal rate",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.sim.gameLoop)
                                     return "resume: game loop not available";
                                 ctx.sim.gameLoop->setRate(TimeRate::Normal);
                                 if (ctx.rcon.shell)
                                     ctx.rcon.shell->print("simulation resumed");
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
        "       | add <id> <entityIdx> [slot] | order <id> <command> [--member <idx>] [--cascade]\n"
        "       | disband <id>   -- formations and the chain of command; `order` commands are the six "
        "wingman commands (attack_my_target, engage_bandits, rejoin, cover_me, hold_fire, return_to_base)",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop || !ctx.sim.entityManager)
                return "flight: not available";
            if (args.empty())
                return "usage: flight list | create | add | order | disband";

            const std::string_view sub = args[0];

            if (sub == "list") {
                ctx.sim.gameLoop->enqueueSimCallback([ctx]() {
                    const auto& reg = ctx.sim.broadcaster->formations();
                    if (reg.size() == 0) {
                        printAdmin(ctx, "[admin] no formations");
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
                        printAdmin(ctx, m);
                        for (const fl::FormationMember& mem : f.members) {
                            char mm[192];
                            std::snprintf(mm, sizeof(mm), "[admin]   member entity=%u slot=%u %s%s", mem.id.index,
                                          mem.slotIndex, mem.isAi() ? "AI" : "HUMAN",
                                          mem.weaponsHold ? " [weapons hold]" : "");
                            printAdmin(ctx, mm);
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

                ctx.sim.gameLoop->enqueueSimCallback([ctx, anchorIdx, commander, parent, callsign]() {
                    fl::EntityId anchor;
                    ctx.sim.entityManager->forEach([&](const fl::EntityState& s) {
                        if (!anchor.valid() && s.id.index == anchorIdx && !s.dead)
                            anchor = s.id;
                    });
                    char m[192];
                    if (!anchor.valid()) {
                        std::snprintf(m, sizeof(m), "[admin] flight create: no live entity with index %u", anchorIdx);
                        printAdmin(ctx, m);
                        return;
                    }
                    const fl::FormationId fid = ctx.sim.broadcaster->formations().create(
                        callsign, anchor, commander, static_cast<fl::FormationId>(parent));
                    if (fid == fl::kNoFormation) {
                        printAdmin(ctx, "[admin] flight create: failed (unknown parent, or tree too deep)");
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
                    printAdmin(ctx, m);
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

                ctx.sim.gameLoop->enqueueSimCallback([ctx, fid, entIdx, slot]() {
                    fl::EntityId ent;
                    ctx.sim.entityManager->forEach([&](const fl::EntityState& s) {
                        if (!ent.valid() && s.id.index == entIdx && !s.dead)
                            ent = s.id;
                    });
                    char m[192];
                    if (!ent.valid()) {
                        std::snprintf(m, sizeof(m), "[admin] flight add: no live entity with index %u", entIdx);
                        printAdmin(ctx, m);
                        return;
                    }

                    // Is a PERSON flying this? Resolve it against the live peer map rather than
                    // EntityState::ownerId, whose "0 = server/AI" convention collides with peer id 0
                    // — an ordinary player. Getting this wrong would mean the server retasks a live
                    // player's aircraft with an autopilot.
                    uint32_t ownerPeer = fl::kNoPeer;
                    ctx.sim.broadcaster->forEachPeer([&](const fl::PeerInfo& pi) {
                        if (pi.eid == ent)
                            ownerPeer = pi.peerId;
                    });

                    fl::FormationMember mem{};
                    mem.id = ent;
                    mem.peerId = ownerPeer; // a player's aircraft joins as a HUMAN member: orders are
                                            // relayed to them as radio calls, never applied to them
                    mem.slotIndex = slot;
                    if (!ctx.sim.broadcaster->formations().addMember(static_cast<fl::FormationId>(fid), mem)) {
                        std::snprintf(m, sizeof(m), "[admin] flight add: no such flight %u", fid);
                        printAdmin(ctx, m);
                        return;
                    }
                    std::snprintf(m, sizeof(m), "[admin] added entity %u to flight %u as %s (slot %u)", entIdx, fid,
                                  ownerPeer == 0 ? "AI" : "HUMAN", slot);
                    printAdmin(ctx, m);
                });
                return "flight add: queued";
            }

            if (sub == "order") {
                if (args.size() < 3)
                    return "usage: flight order <flightId> <command> [--member <entityIdx>] [--cascade]";
                uint32_t fid = 0;
                if (!parseU32(args[1], fid))
                    return "flight order: invalid flight id";
                const std::optional<fl::ai::WingmanCommand> cmd = fl::ai::parseWingmanCommand(args[2]);
                if (!cmd)
                    return "flight order: unknown command (attack_my_target, engage_bandits, rejoin, cover_me, "
                           "hold_fire, return_to_base)";

                uint32_t memberIdx = fl::kFlightAll;
                bool cascade = false;
                for (std::size_t i = 3; i < args.size(); ++i) {
                    if (args[i] == "--cascade")
                        cascade = true;
                    else if (args[i] == "--member" && i + 1 < args.size())
                        (void)parseU32(args[++i], memberIdx);
                }

                // The game master has no boresight, so attack_my_target cannot designate through this
                // path. Say so rather than silently degrading the order to "hold station".
                if (*cmd == fl::ai::WingmanCommand::AttackMyTarget)
                    return "flight order: attack_my_target needs a commander's boresight, which the console does not "
                           "have -- use `spawn --ai pursuit <idx>` to point an AI at a specific entity";

                const auto ordinal = static_cast<uint8_t>(*cmd);
                ctx.sim.gameLoop->enqueueSimCallback([ctx, fid, ordinal, memberIdx, cascade]() {
                    const auto rep = ctx.sim.broadcaster->applyFlightOrder(static_cast<fl::FormationId>(fid), ordinal,
                                                                           memberIdx, cascade);
                    char m[192];
                    std::snprintf(m, sizeof(m), "[admin] flight %u ordered %s: %d AI retasked, %d relayed to players%s",
                                  fid, std::string(fl::ai::kWingmanCommandNames[ordinal]).c_str(), rep.aiRetasked,
                                  rep.humansRelayed, rep.deadSkipped > 0 ? " (some members are dead)" : "");
                    printAdmin(ctx, m);
                });
                return "flight order: queued";
            }

            if (sub == "disband") {
                if (args.size() < 2)
                    return "usage: flight disband <flightId>";
                uint32_t fid = 0;
                if (!parseU32(args[1], fid))
                    return "flight disband: invalid flight id";
                ctx.sim.gameLoop->enqueueSimCallback([ctx, fid]() {
                    // Children are re-parented, not destroyed: disbanding a package must not delete
                    // the flights inside it, and the aircraft keep flying whatever they were last told.
                    const bool ok = ctx.sim.broadcaster->formations().destroy(static_cast<fl::FormationId>(fid));
                    char m[128];
                    std::snprintf(m, sizeof(m), ok ? "[admin] disbanded flight %u" : "[admin] no such flight %u", fid);
                    printAdmin(ctx, m);
                });
                return "flight disband: queued";
            }

            return "usage: flight list | create | add | order | disband";
        });

    registry.registerCommand("quit", "quit  -- shut down fl-server gracefully",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.env.quitFlag)
                                     return "quit: not available";
                                 *ctx.env.quitFlag = 1;
                                 return "shutting down...";
                             });
}

} // namespace fl