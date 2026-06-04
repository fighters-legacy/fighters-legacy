// SPDX-License-Identifier: GPL-3.0-or-later
#include "LocalServer.h"

#include "ILogger.h"
#include "Subprocess.h"
#include "debug/DebugCommands.h"
#include "entity/EntityTypeRegistry.h"
#include "render/SimRenderBridge.h"
#include "weather/WeatherController.h"
#include "weather/WeatherTypes.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Binary discovery
// ---------------------------------------------------------------------------

// Returns the stem path for fl-server (without extension).
// In development builds FL_SERVER_DEV_PATH is injected by CMake as an absolute
// path to the build-tree binary; in release packages fl-server is co-located
// with the game binary found via SDL_GetBasePath().
static std::string findServerStem() {
#ifdef FL_SERVER_DEV_PATH
    // Dev build: absolute path embedded by CMake generator expression.
    return FL_SERVER_DEV_PATH;
#else
    // Release: fl-server is installed alongside the game binary.
    return std::string(SDL_GetBasePath()) + "fl-server";
#endif
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct LocalServer::Impl {
    // unique_ptr<Subprocess> rather than a value member: the value form caused
    // LocalServer.cpp to require Subprocess::Impl to be complete (GCC pimpl chain).
    // unique_ptr<Subprocess> only requires Subprocess to be complete here, which it is.
    std::unique_ptr<Subprocess> sub;
    std::string sessionToken; // 24-char hex token generated at start(); passed to fl-server via --admin-token
};

// ---------------------------------------------------------------------------
// LocalServer
// ---------------------------------------------------------------------------

LocalServer::LocalServer(ILogger& log) : m_log(log) {}

// Explicit destructor defined here so the compiler sees Impl as a complete type
// when unique_ptr<Impl> is destroyed (pimpl pattern requirement).
LocalServer::~LocalServer() {
    if (m_impl && m_impl->sub && m_impl->sub->valid())
        m_impl->sub->stop();
}

bool LocalServer::start(const char* bindAddr, uint16_t port) {
    m_impl = std::make_unique<Impl>();

    std::string stem = findServerStem();

    // Generate a per-session admin token (24 hex chars) and pass it to fl-server via
    // --admin-token so MsgAdminCommand authentication uses the network path even in
    // single-player mode. This retires the stdin pipe path for debug commands.
    {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 15);
        static constexpr char kHex[] = "0123456789abcdef";
        m_impl->sessionToken.reserve(24);
        for (int i = 0; i < 24; ++i)
            m_impl->sessionToken += kHex[dist(rng)];
    }

    char portStr[8], maxPeersStr[4];
    std::snprintf(portStr, sizeof(portStr), "%u", port);
    std::snprintf(maxPeersStr, sizeof(maxPeersStr), "1");

    std::vector<std::string> args{portStr, maxPeersStr, "--bind", bindAddr, "--admin-token", m_impl->sessionToken};

    // Spawn into a unique_ptr<Subprocess> to avoid Subprocess::Impl completion
    // requirements in LocalServer.cpp (pimpl isolation via pointer indirection).
    m_impl->sub = std::make_unique<Subprocess>(
        Subprocess::spawn(stem, args, /*captureStdout=*/true, /*captureStdin=*/true, m_log));
    if (!m_impl->sub->valid()) {
        m_log.log(LogLevel::Error, __FILE__, __LINE__, "LocalServer: failed to spawn fl-server subprocess");
        return false;
    }

    // Wait for fl-server to log "listening on" (ready) or "bind failed" (error).
    // Timeout: 3 seconds.
    while (m_impl->sub->isRunning()) {
        auto line = m_impl->sub->readStdoutLine(3000);
        if (!line)
            break; // timeout
        // Echo to the game log so developers can see server startup messages.
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "[fl-server] %s", line->c_str());
            m_log.log(LogLevel::Info, __FILE__, __LINE__, buf);
        }
        if (line->find("listening on") != std::string::npos)
            return true;
        if (line->find("bind failed") != std::string::npos || line->find("network init failed") != std::string::npos) {
            m_log.log(LogLevel::Error, __FILE__, __LINE__, "LocalServer: fl-server failed to bind — see above");
            return false;
        }
    }

    m_log.log(LogLevel::Error, __FILE__, __LINE__, "LocalServer: fl-server did not become ready within 3 s");
    return false;
}

std::string_view LocalServer::sessionToken() const {
    return m_impl ? std::string_view(m_impl->sessionToken) : std::string_view{};
}

void LocalServer::stop() {
    if (m_impl && m_impl->sub && m_impl->sub->valid())
        m_impl->sub->stop();
}

bool LocalServer::isRunning() const {
    return m_impl && m_impl->sub && m_impl->sub->isRunning();
}

EnvironmentState LocalServer::initialEnvironment() const {
    // Return a sensible default matching fl-server's startup defaults
    // (PartlyCloudy, 09:00) before MsgWeatherState has arrived.
    EnvironmentState env{};
    float tod = 9.0f;
    fl::WeatherController::applyPresetToEnv(fl::WeatherPreset::PartlyCloudy, tod, env);
    env.timeOfDay = tod;
    return env;
}

void LocalServer::registerDebugCommands(DebugCommandRegistry& registry,
                                        std::function<void(std::string_view)> serverCommand,
                                        fl::SimRenderBridge& renderBridge, fl::EntityTypeRegistry* typeRegistry,
                                        uint32_t* playerEntityIdx, uint32_t* playerEntityGen, bool* showPos) {
    DebugCommandContext ctx{};
    ctx.renderBridge = &renderBridge;
    ctx.typeRegistry = typeRegistry;
    ctx.playerEntityIdx = playerEntityIdx;
    ctx.playerEntityGen = playerEntityGen;
    ctx.showPos = showPos;
    ctx.serverCommand = std::move(serverCommand);
    registerBuiltinCommands(registry, ctx);
}
