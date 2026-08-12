// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

namespace fl {

// ServerRuntime (#1084) — fl-server's lifetime, in named phases.
//
// `main()` was a 2,680-line function holding ~70 objects in function scope whose only lifetime
// specification was a comment: "Destruction order (LIFO): gameLoop first..., then rconServer..., then
// adminShell, then adminRegistry". Teardown order in that file is load-bearing and has already
// produced bugs -- #1054 was a double-free from voice capture being destroyed after SDL_Quit(), and
// #1038 replaced a stdin reader that deadlocked exit. A comment is not a mechanism for that. (It was
// not even an accurate one: `stdinReader` and `replayRecorder` are declared *after* `gameLoop`, so
// they are destroyed *before* it, not after.)
//
// Here the objects are MEMBERS of `Impl`, so their declaration order is the teardown contract and the
// language enforces it. That order is deliberately the same order they were declared in `main()`:
// today's teardown sequence is the one that works, and this change is about making it checkable, not
// about changing it.
//
// The client already solved this: `Game` is a pimpl facade with named initPlatform /
// initWindowAndRenderer / initContent / initGameSystems phases, and it is the in-repo template.
class ServerRuntime {
  public:
    // The command line, already parsed. Kept as data so `main()` is argument parsing and nothing
    // else, and so the embedded single-player launcher can drive the runtime without building an
    // argv it would only have to parse back.
    struct Options {
        std::string bind;          // --bind: overrides server.toml and FL_BIND_ADDRESS
        std::string adminToken;    // --admin-token: internal single-player use
        std::string transport;     // --transport <gns|enet>: overrides [network]
        std::string metricsJson;   // --metrics-json: overrides [metrics]
        std::string replayDir;     // --replay-dir: overrides [replay] dir
        std::string replayHashLog; // --replay-hash-log: the #644 per-tick state-hash sidecar
        std::string assets;        // --assets: content root holding mods/
        std::string mission;       // --mission <name>: overrides [rotation]
        std::string missionReport; // --mission-report <path>: run headless, write JSON, exit (#856)
        std::string campaign;      // --campaign <file>: run the campaign's next sortie (#584)
        std::string timeRate;      // --time-rate <name>: reduced wall-rate for recording (#915)
        int testSpawnAi{-1};       // --test-spawn-ai-count; >= 0 overrides [world]
        long simWorkers{-1};       // --sim-worker-threads; >= 0 overrides [world]
        long flightSize{-1};       // --flight-size; >= 0 overrides [flight] size
        bool noDiscovery{false};   // --no-discovery: bind no LAN sockets at all (#1054)
        // Still needed by the positional-argument override pass ([port] [maxPeers]), which is Tier 2
        // of the documented three-tier precedence and belongs with the other two.
        int argc{0};
        char** argv{nullptr};
    };

    explicit ServerRuntime(Options opts);
    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // Run every init phase in order, then the main loop; returns the process exit code. A phase that
    // fails logs the reason and stops the sequence. A phase may also finish the process early and
    // successfully (`--mission-report`), which is why this returns a code rather than a bool.
    [[nodiscard]] int run();

  private:
    Options m_opts; // declared first: Impl holds a reference to it
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
