// SPDX-License-Identifier: GPL-3.0-or-later
#include "AdminConsole.h"

#include "server_config.h"
#include <ILogger.h>
#include <debug/DebugCommandRegistry.h>
#include <entity/EntityManager.h>
#include <entity/EntityState.h>
#include <loop/GameLoop.h>
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

// Returns true when arg consists entirely of ASCII digits (treat as peerId).
static bool isNumeric(std::string_view arg) {
    if (arg.empty())
        return false;
    for (char c : arg)
        if (c < '0' || c > '9')
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// registerServerCommands
// ---------------------------------------------------------------------------

void registerServerCommands(DebugCommandRegistry& registry, ServerCommandContext ctx) {

    // help [command]
    registry.registerCommand("help", "help [command]  -- list all commands or show usage for one",
                             [&registry](std::span<std::string_view> args) -> std::string {
                                 if (!args.empty())
                                     return registry.helpFor(args[0]);
                                 return registry.helpText();
                             });

    // status
    registry.registerCommand("status", "status  -- show server state (uptime, peer count, entity count, tick rate)",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.broadcaster || !ctx.entityManager)
                                     return "status: not available";
                                 using namespace std::chrono;
                                 auto uptimeSec = duration_cast<seconds>(steady_clock::now() - ctx.startTime).count();
                                 int peers = ctx.broadcaster->getPeerCount();
                                 uint32_t entities = ctx.entityManager->liveCount();
                                 char buf[256];
                                 std::snprintf(buf, sizeof(buf), "uptime: %llds  peers: %d  entities: %u  tick: 60 Hz",
                                               static_cast<long long>(uptimeSec), peers, entities);
                                 return buf;
                             });

    // peers
    registry.registerCommand(
        "peers", "peers  -- list connected peers (peerId, address, entity index/generation)",
        [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx.broadcaster || !ctx.gameLoop)
                return "peers: not available";
            ctx.gameLoop->enqueueSimCallback([ctx]() {
                int count = 0;
                ctx.broadcaster->forEachPeer([&](uint32_t peerId, const std::string& addr, fl::EntityId eid) {
                    std::printf("[admin] peer %u  %s  entity=%u/%u\n", peerId, addr.c_str(), eid.index, eid.generation);
                    ++count;
                });
                if (count == 0)
                    std::printf("[admin] peers: no connected peers\n");
                std::fflush(stdout);
            });
            return {};
        });

    // kick <peerId|IP>
    registry.registerCommand("kick", "kick <peerId|IP>  -- disconnect a peer by ID or all peers from an IP address",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: kick <peerId|IP>";
                                 if (!ctx.broadcaster || !ctx.gameLoop)
                                     return "kick: not available";
                                 std::string arg(args[0]);
                                 if (isNumeric(arg)) {
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return "kick: invalid peer ID";
                                     ctx.gameLoop->enqueueSimCallback([ctx, peerId]() {
                                         ctx.broadcaster->kickPeer(peerId);
                                         std::printf("[admin] kicked peer %u\n", peerId);
                                         std::fflush(stdout);
                                     });
                                 } else {
                                     std::string ip = normalizeIp(arg);
                                     ctx.gameLoop->enqueueSimCallback([ctx, ip]() {
                                         int kicked = 0;
                                         ctx.broadcaster->forEachPeer(
                                             [&](uint32_t peerId, const std::string& addr, fl::EntityId) {
                                                 if (extractIp(addr) == ip) {
                                                     ctx.broadcaster->kickPeer(peerId);
                                                     ++kicked;
                                                 }
                                             });
                                         std::printf("[admin] kicked %d peer(s) from IP %s\n", kicked, ip.c_str());
                                         std::fflush(stdout);
                                     });
                                 }
                                 return {};
                             });

    // ban <peerId|IP>
    registry.registerCommand("ban", "ban <peerId|IP>  -- add IP to in-memory ban list and kick matching peers",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: ban <peerId|IP>";
                                 if (!ctx.broadcaster || !ctx.gameLoop)
                                     return "ban: not available";
                                 std::string arg(args[0]);
                                 if (isNumeric(arg)) {
                                     uint32_t peerId = 0;
                                     auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), peerId);
                                     if (ec != std::errc{})
                                         return "ban: invalid peer ID";
                                     ctx.gameLoop->enqueueSimCallback([ctx, peerId]() {
                                         std::string foundIp;
                                         ctx.broadcaster->forEachPeer(
                                             [&](uint32_t pid, const std::string& addr, fl::EntityId) {
                                                 if (pid == peerId)
                                                     foundIp = extractIp(addr);
                                             });
                                         if (foundIp.empty()) {
                                             std::printf("[admin] ban: peer %u not found\n", peerId);
                                         } else {
                                             ctx.broadcaster->banAddress(foundIp);
                                             std::printf("[admin] banned IP %s (peer %u)\n", foundIp.c_str(), peerId);
                                         }
                                         std::fflush(stdout);
                                     });
                                 } else {
                                     std::string ip = normalizeIp(arg);
                                     ctx.gameLoop->enqueueSimCallback([ctx, ip]() {
                                         ctx.broadcaster->banAddress(ip);
                                         std::printf("[admin] banned IP %s\n", ip.c_str());
                                         std::fflush(stdout);
                                     });
                                 }
                                 return {};
                             });

    // unban <IP>
    registry.registerCommand("unban", "unban <IP>  -- remove an IP from the in-memory ban list",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: unban <IP>";
                                 if (!ctx.broadcaster || !ctx.gameLoop)
                                     return "unban: not available";
                                 std::string ip = normalizeIp(args[0]);
                                 ctx.gameLoop->enqueueSimCallback([ctx, ip]() {
                                     ctx.broadcaster->unbanAddress(ip);
                                     std::printf("[admin] unbanned IP %s\n", ip.c_str());
                                     std::fflush(stdout);
                                 });
                                 return {};
                             });

    // set_weather <preset>
    registry.registerCommand(
        "set_weather", "set_weather <clear|partly_cloudy|overcast|rain|storm>  -- change weather preset",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: set_weather <clear|partly_cloudy|overcast|rain|storm>";
            if (!ctx.weatherController || !ctx.gameLoop)
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
            ctx.gameLoop->enqueueSimCallback([ctx, preset]() { ctx.weatherController->setPreset(preset); });
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
                                 if (!ctx.weatherController || !ctx.gameLoop)
                                     return "set_time: not available";
                                 ctx.gameLoop->enqueueSimCallback(
                                     [ctx, hours]() { ctx.weatherController->setTimeOfDay(hours); });
                                 char buf[64];
                                 std::snprintf(buf, sizeof(buf), "set_time: %.2f", hours);
                                 return buf;
                             });

    // spawn <type> <x> <y> <z>
    registry.registerCommand(
        "spawn", "spawn <type> <x> <y> <z>  -- spawn a registered entity type at world position",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.size() < 4)
                return "usage: spawn <type> <x> <y> <z>";
            if (!ctx.entityManager || !ctx.gameLoop)
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
            ctx.gameLoop->enqueueSimCallback([ctx, typeId, x, y, z]() {
                fl::EntityTransform t{};
                t.pos[0] = x;
                t.pos[1] = y;
                t.pos[2] = z;
                fl::EntityId id = ctx.entityManager->spawn(typeId.c_str(), t);
                if (id.valid())
                    std::printf("[admin] spawned %s entity=%u/%u\n", typeId.c_str(), id.index, id.generation);
                else
                    std::printf("[admin] spawn: type '%s' unknown or cap reached\n", typeId.c_str());
                std::fflush(stdout);
            });
            return {};
        });

    // kill <idx>
    registry.registerCommand("kill", "kill <idx>  -- remove a live entity by pool index (see 'peers' or 'entities')",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.empty())
                                     return "usage: kill <idx>";
                                 if (!ctx.entityManager || !ctx.gameLoop)
                                     return "kill: not available";
                                 uint32_t targetIdx = 0;
                                 auto [ptr, ec] =
                                     std::from_chars(args[0].data(), args[0].data() + args[0].size(), targetIdx);
                                 if (ec != std::errc{})
                                     return "kill: invalid index";
                                 ctx.gameLoop->enqueueSimCallback([ctx, targetIdx]() {
                                     fl::EntityId killId;
                                     ctx.entityManager->forEach([&](const fl::EntityState& state) {
                                         if (!killId.valid() && state.id.index == targetIdx)
                                             killId = state.id;
                                     });
                                     if (killId.valid()) {
                                         ctx.entityManager->kill(killId);
                                         std::printf("[admin] killed entity %u/%u\n", killId.index, killId.generation);
                                     } else {
                                         std::printf("[admin] kill: no live entity with index %u\n", targetIdx);
                                     }
                                     std::fflush(stdout);
                                 });
                                 return {};
                             });

    // reload_config
    registry.registerCommand("reload_config", "reload_config  -- re-read server.toml and apply: name (beacon), motd",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.configPath || ctx.configPath->empty())
                                     return "reload_config: not available";
                                 std::ifstream f(*ctx.configPath);
                                 if (!f)
                                     return "reload_config: cannot open " + *ctx.configPath;
                                 std::ostringstream ss;
                                 ss << f.rdbuf();
                                 ServerConfig newCfg = parseServerConfig(ss.str(), ctx.logger);
                                 if (ctx.beacon)
                                     ctx.beacon->setName(newCfg.name);
                                 return "reload_config: name=\"" + newCfg.name + "\"  motd=\"" + newCfg.motd +
                                        "\"  (other fields require restart)";
                             });

    // quit
    registry.registerCommand("quit", "quit  -- shut down fl-server gracefully",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.quitFlag)
                                     return "quit: not available";
                                 *ctx.quitFlag = 1;
                                 return "shutting down...";
                             });
}
