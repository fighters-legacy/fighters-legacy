// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl-server's entry point: parse the command line, hand it to ServerRuntime, return its exit code.
//
// Everything else — ~70 owned objects, the init sequence and the teardown order — lives in
// ServerRuntime (#1084). This file used to hold all of it in one 2,680-line function whose lifetime
// contract was a comment.
#include "ServerRuntime.h"

#include "Version.h"            // FL_VERSION_STRING, for --version
#include <net/NetworkFactory.h> // networkBackendVersion + TransportKind, for --version

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace fl;

int main(int argc, char** argv) {
    fl::ServerRuntime::Options opts;
    opts.argc = argc;
    opts.argv = argv;

    // Pre-pass: --help / --version / --bind

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "Usage: fl-server [port] [maxPeers]\n"
                "\n"
                "Options:\n"
                "  --help             Print this message and exit\n"
                "  --version          Print version and exit\n"
                "  --bind <addr>      Bind address (overrides server.toml and FL_BIND_ADDRESS)\n"
                "  --assets <dir>     Content root holding mods/ (overrides FL_ASSETS_ROOT and the CWD)\n"
                "  --metrics-json <p> Write the per-phase tick-budget JSON to <p> (overrides [metrics])\n"
                "  --replay-dir <p>   Write .flrep recordings to <p> (overrides [replay] dir)\n"
                "  --replay-hash-log <p>  Write the per-tick replay state-hash sidecar to <p> (#644)\n"
                "  --test-spawn-ai-count <n>  Pre-spawn n loiter-AI entities (overrides [world])\n"
                "  --sim-worker-threads <n>  Sim-tick CPU parallelism; 0=auto, 1=serial (overrides [world])\n"
                "  --flight-size <n>         AI wingmen per player; 0=none (overrides [flight])\n"
                "  --no-discovery            Bind no LAN sockets: no discovery beacon, no query\n"
                "                            responder (overrides [discovery]; used by single-player)\n"
                "  --mission <name>          Load a mission at startup (overrides [rotation])\n"
                "  --mission-report <path>   Run the mission headless to completion, write a JSON outcome, exit\n"
                "  --time-rate <name>        Sim wall-clock rate: paused|eighth|quarter|half|normal|double|quad|octa\n"
                "                            (sim dt stays 1/60; slows serving for a slow recording client, #915)\n"
                "\n"
                "Admin console commands are available on stdin (type 'help' for a command list).\n"
                "\n"
                "Environment:\n"
                "  FL_CONFIG              Path to server.toml (default: ./server.toml)\n"
                "  FL_PORT                Bind port (default: 4778)\n"
                "  FL_BIND_ADDRESS        Bind address (default: 0.0.0.0)\n"
                "  FL_ASSETS_ROOT         Content root holding mods/ (default: current directory)\n"
                "  FL_MAX_PEERS           Max simultaneous peers (default: 32)\n"
                "  FL_NAME                Server name (default: \"Unnamed Server\")\n"
                "  FL_LOBBY_REGISTER      \"true\" to advertise to fl-lobby, Phase 2\n"
                "  FL_LOBBY_URL           fl-lobby base URL, Phase 2\n"
                "  FL_LOBBY_VISIBILITY    \"public\" or \"private\", Phase 2\n"
                "  FL_AI_DIFFICULTY_FLOOR recruit/cadet/veteran/ace, Phase 2\n"
                "  FL_OPERATOR_PASSWORD    Operator password for network admin commands (overrides server.toml)\n"
                "\n"
                "Config file is written with defaults on first run if absent.\n"
                "See docs/server-ops/server-config.md for the full operator reference.\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("fl-server %s (%s)\n", FL_VERSION_STRING, networkBackendVersion(TransportKind::Gns));
            return 0;
        }
        if (std::strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            opts.bind = argv[++i];
        if (std::strcmp(argv[i], "--transport") == 0 && i + 1 < argc)
            opts.transport = argv[++i];
        if (std::strcmp(argv[i], "--admin-token") == 0 && i + 1 < argc)
            opts.adminToken = argv[++i];
        if (std::strcmp(argv[i], "--metrics-json") == 0 && i + 1 < argc)
            opts.metricsJson = argv[++i];
        if (std::strcmp(argv[i], "--replay-dir") == 0 && i + 1 < argc)
            opts.replayDir = argv[++i];
        if (std::strcmp(argv[i], "--replay-hash-log") == 0 && i + 1 < argc)
            opts.replayHashLog = argv[++i];
        if (std::strcmp(argv[i], "--test-spawn-ai-count") == 0 && i + 1 < argc)
            opts.testSpawnAi = std::atoi(argv[++i]);
        if (std::strcmp(argv[i], "--assets") == 0 && i + 1 < argc)
            opts.assets = argv[++i];
        if (std::strcmp(argv[i], "--mission") == 0 && i + 1 < argc)
            opts.mission = argv[++i];
        if (std::strcmp(argv[i], "--mission-report") == 0 && i + 1 < argc)
            opts.missionReport = argv[++i];
        if (std::strcmp(argv[i], "--campaign") == 0 && i + 1 < argc)
            opts.campaign = argv[++i];
        if (std::strcmp(argv[i], "--time-rate") == 0 && i + 1 < argc)
            opts.timeRate = argv[++i];
        if (std::strcmp(argv[i], "--sim-worker-threads") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (end != argv[i] && n >= 0 && n <= 256)
                opts.simWorkers = n;
        }
        if (std::strcmp(argv[i], "--flight-size") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (end != argv[i] && n >= 0 && n <= 8)
                opts.flightSize = n;
        }
        // #1054: suppress BOTH LAN-facing sockets — the discovery beacon and the query responder.
        // The client's embedded single-player server passes this: it serves one loopback peer, so
        // advertising it on the LAN is wrong, and its query responder would bind game port + 1 and
        // squat a port belonging to a dedicated server.
        if (std::strcmp(argv[i], "--no-discovery") == 0)
            opts.noDiscovery = true;
    }

    fl::ServerRuntime runtime(std::move(opts));
    return runtime.run();
}
