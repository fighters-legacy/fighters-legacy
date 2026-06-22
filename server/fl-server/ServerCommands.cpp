// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCommands.h"

#include "ai/AiControllerFactory.h"
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

static void printAuthSection(const char* label, const fl::AuthLockoutSummary& s, CommandShell* shell) {
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "[admin] %s:", label);
    std::printf("%s\n", hdr);
    if (shell)
        shell->print(hdr);
    for (const auto& e : s.entries) {
        char m[192];
        if (e.lockedOut)
            std::snprintf(m, sizeof(m), "[admin]   %-37s locked out -- expires in %s", e.ip.c_str(),
                          formatSecs(e.expiresIn).c_str());
        else
            std::snprintf(m, sizeof(m), "[admin]   %-37s %d failure(s) (threshold: %d)", e.ip.c_str(), e.failures,
                          s.threshold);
        std::printf("%s\n", m);
        if (shell)
            shell->print(m);
    }
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
            char buf[256];
            std::snprintf(buf, sizeof(buf), "uptime: %llds  peers: %d  entities: %u  tick: 60 Hz",
                          static_cast<long long>(uptimeSec), peers, entities);
            std::string out(buf);
            auto ls = ctx.sim.broadcaster->getAuthLockoutSummary();
            if (ls.activeCount > 0) {
                char lbuf[96];
                std::snprintf(lbuf, sizeof(lbuf),
                              "\nadmin auth lockouts: %d active (use admin_auth_status for details)", ls.activeCount);
                out += lbuf;
            }
            return out;
        });

    // peers
    registry.registerCommand(
        "peers", "peers  -- list connected peers (peerId, address, entity index/generation, one-way delay)",
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx.sim.broadcaster || !ctx.sim.gameLoop)
                return "peers: not available";
            ctx.sim.gameLoop->enqueueSimCallback([ctx]() {
                int count = 0;
                ctx.sim.broadcaster->forEachPeer(
                    [&](uint32_t peerId, const std::string& addr, fl::EntityId eid, uint32_t delayTicks) {
                        char m[256];
                        std::snprintf(m, sizeof(m), "[admin] peer %u  %s  entity=%u/%u  delay=%ut (~%ums)", peerId,
                                      addr.c_str(), eid.index, eid.generation, delayTicks,
                                      (delayTicks * 1000u + 30u) / 60u);
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
                                         ctx.sim.broadcaster->forEachPeer(
                                             [&](uint32_t peerId, const std::string& addr, fl::EntityId, uint32_t) {
                                                 if (extractIp(addr) == ip) {
                                                     ctx.sim.broadcaster->kickPeer(peerId);
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
                                         ctx.sim.broadcaster->forEachPeer(
                                             [&](uint32_t pid, const std::string& addr, fl::EntityId, uint32_t) {
                                                 if (pid == peerId)
                                                     foundIp = extractIp(addr);
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

                                 printAuthSection("MsgAdminCommand channel", adminS, ctx.rcon.shell);
                                 if (hasRcon) {
                                     std::printf("\n");
                                     if (ctx.rcon.shell)
                                         ctx.rcon.shell->print("");
                                     printAuthSection("RCON channel", rconS, ctx.rcon.shell);
                                 }
                                 std::fflush(stdout);

                                 char ackBuf[80];
                                 if (hasRcon)
                                     std::snprintf(ackBuf, sizeof(ackBuf), "admin: %d lockout(s) | rcon: %d lockout(s)",
                                                   adminS.activeCount, rconS.activeCount);
                                 else
                                     std::snprintf(ackBuf, sizeof(ackBuf), "%d lockout(s) active", adminS.activeCount);
                                 return std::string(ackBuf);
                             });

    // set_weather <preset>
    registry.registerCommand(
        "set_weather", "set_weather <clear|partly_cloudy|overcast|rain|storm>  -- change weather preset",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: set_weather <clear|partly_cloudy|overcast|rain|storm>";
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
            else
                return "set_weather: unknown preset (clear|partly_cloudy|overcast|rain|storm)";
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

    // spawn <type> <x> <y> <z> [--ai <behavior> [behavior-args...]]
    registry.registerCommand(
        "spawn", "spawn <type> <x> <y> <z> [--ai <behavior> [args...]]  -- spawn entity with optional AI controller",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 4)
                return "usage: spawn <type> <x> <y> <z> [--ai <behavior> [args...]]";
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

            // Parse optional --ai <behavior> [behavior-args...]
            std::string behavior;
            std::vector<std::string> behaviorArgStrings;
            for (std::size_t i = 4; i < args.size(); ++i) {
                if (args[i] == "--ai") {
                    if (i + 1 >= args.size())
                        return "spawn: --ai requires a behavior name";
                    behavior = std::string(args[++i]);
                    while (i + 1 < args.size())
                        behaviorArgStrings.emplace_back(args[++i]);
                    break;
                }
            }

            ctx.sim.gameLoop->enqueueSimCallback([ctx, typeId, x, y, z, behavior, behaviorArgStrings]() {
                fl::EntityTransform t{};
                t.pos[0] = x;
                t.pos[1] = y;
                t.pos[2] = z;
                fl::EntityId id = ctx.sim.entityManager->spawn(typeId.c_str(), t);
                char m[160];
                if (id.valid()) {
                    std::snprintf(m, sizeof(m), "[admin] spawned %s entity=%u/%u", typeId.c_str(), id.index,
                                  id.generation);
                    std::printf("%s\n", m);
                    if (ctx.rcon.shell)
                        ctx.rcon.shell->print(m);

                    if (!behavior.empty() && ctx.sim.broadcaster) {
                        // Rebuild string_views from the captured strings (safe — strings owned by capture).
                        std::vector<std::string_view> argViews;
                        argViews.reserve(behaviorArgStrings.size());
                        for (const auto& s : behaviorArgStrings)
                            argViews.push_back(s);

                        auto ctrl = fl::ai::createController(behavior, std::span<std::string_view>(argViews),
                                                             ctx.sim.entityManager);
                        if (ctrl) {
                            ctx.sim.broadcaster->registerController(id, std::move(ctrl));
                            char am[128];
                            std::snprintf(am, sizeof(am), "[admin] attached AI '%s' to entity=%u", behavior.c_str(),
                                          id.index);
                            std::printf("%s\n", am);
                            if (ctx.rcon.shell)
                                ctx.rcon.shell->print(am);
                        } else {
                            char wm[128];
                            std::snprintf(wm, sizeof(wm), "[admin] spawn: unknown AI behavior '%s' or bad args",
                                          behavior.c_str());
                            std::printf("%s\n", wm);
                            if (ctx.rcon.shell)
                                ctx.rcon.shell->print(wm);
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
            if (behavior.empty()) {
                std::snprintf(spawnBuf, sizeof(spawnBuf), "spawn: queued type %s at %.1f %.1f %.1f", typeId.c_str(),
                              static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            } else {
                std::snprintf(spawnBuf, sizeof(spawnBuf), "spawn: queued type %s at %.1f %.1f %.1f --ai %s",
                              typeId.c_str(), static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                              behavior.c_str());
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

    // reload_config
    registry.registerCommand("reload_config",
                             "reload_config  -- re-read server.toml and apply: name (beacon), motd, motd_display_s,"
                             " draw_distance_km, baseline_interval_ticks (other fields require restart)",
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
                                     auto newBaseline = newCfg.baselineIntervalTicks;
                                     ctx.sim.gameLoop->enqueueSimCallback(
                                         [ctx, newMotd, newMotdDisplayS, newDraw, newBaseline]() mutable {
                                             ctx.sim.broadcaster->setMotd(std::move(newMotd));
                                             ctx.sim.broadcaster->setMotdDisplaySeconds(newMotdDisplayS);
                                             ctx.sim.broadcaster->setDrawDistance(newDraw);
                                             ctx.sim.broadcaster->setBaselineInterval(newBaseline);
                                         });
                                 }
                                 return "reload_config: name=\"" + newCfg.name + "\"  motd=\"" + newCfg.motd +
                                        "\"  motd_display_s=" + std::to_string(newCfg.motdDisplayS) +
                                        "  draw_distance_km=" + std::to_string(newCfg.drawDistanceKm) +
                                        "  baseline_interval_ticks=" + std::to_string(newCfg.baselineIntervalTicks) +
                                        "  (other fields require restart)";
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
    registry.registerCommand("quit", "quit  -- shut down fl-server gracefully",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.env.quitFlag)
                                     return "quit: not available";
                                 *ctx.env.quitFlag = 1;
                                 return "shutting down...";
                             });
}
