// SPDX-License-Identifier: GPL-3.0-or-later
#include "FileLogger.h"
#include "Platform.h"
#include "Version.h"
#include "config/UserConfig.h"
#include "crash/CrashInfo.h"
#include "crash/CrashReporter.h"
#include "sdl3/SDL3Window.h"
#include "vulkan/VkRenderer.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // Step 0: Early flag handling — before any platform init so they work
    // even when SDL/Vulkan are absent.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("fighters-legacy %s (%s)\n", FL_VERSION_STRING, FL_GIT_HASH);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: fighters-legacy [--log-level debug|info|warn|error] [--version] [--help]\n");
            return 0;
        }
    }

    // Step 1: Resolve UserData directory
    SDL_Init(0);
    char* prefRaw = SDL_GetPrefPath("jomkz", "fighters-legacy");
    fs::path userDataDir = prefRaw ? fs::path(prefRaw) : fs::path(".");
    if (prefRaw)
        SDL_free(prefRaw);

    Platform p;

    // Step 2: FileLogger — open at default level
    auto fileLogger = std::make_unique<FileLogger>();
    if (!fileLogger->open((userDataDir / "logs").string(), 10)) {
        std::fprintf(stderr, "fighters-legacy: cannot open log file in %s, falling back to stderr\n",
                     (userDataDir / "logs").string().c_str());
        // log() calls will silently no-op; startup continues
    }
    FileLogger* rawLogger = fileLogger.get();
    p.logger = std::move(fileLogger);

    // Steps 3–4: UserConfig and ModLoader cannot be wired in Phase 1 — both require
    // a concrete IFilesystem which does not exist yet.
    // TODO(Phase 2): instantiate UserConfig and apply persisted log_level when
    // SDL3Filesystem ships.

    // Step 4: Apply --log-level CLI override (session-only; does not persist)
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--log-level") == 0)
            rawLogger->setMinLevel(parseLogLevel(argv[i + 1]));
    }

    // Step 5: Window (no SDL video init yet; showMessageBox safe before init per SDL3 docs)
    auto window = std::make_unique<SDL3Window>();
    p.window = std::move(window);

    // Step 6: Post-crash dialog
    CrashReporter::checkPreviousCrash(userDataDir.string(), p.window.get(), rawLogger,
                                      "https://github.com/jomkz/fighters-legacy/issues/new");

    // Step 7: CrashReporter — sentinel + signal handlers
    CrashInfo crashInfo;
    crashInfo.engineVersion = FL_VERSION_STRING;
    crashInfo.populateOS();
    CrashReporter crashReporter;
    crashReporter.init(
        {userDataDir.string(), "https://github.com/jomkz/fighters-legacy/issues/new", rawLogger, p.window.get()},
        crashInfo);

    // Step 8: Platform init — window
    if (!p.window->init("Fighters Legacy", 1280, 720)) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "window init failed");
        crashReporter.shutdown();
        return 1;
    }

    // Renderer
    auto renderer = std::make_unique<VkRenderer>();
    p.renderer = std::move(renderer);
    if (!p.renderer->init(p.window.get())) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "renderer init failed");
        crashReporter.shutdown();
        return 1;
    }

    // Audio and input are not wired in the Phase 1 stub (no game loop).
    // TODO(Phase 2): instantiate OALAudio and SDL3Input here.

    // Step 9: GPU info → crash reporter
    crashReporter.setGpuInfo(p.renderer->gpuInfo());

    // Step 10: First-run check
    // TODO(Phase 2): wire FirstRun when UserConfig is wired to SDL3Filesystem.

    // Step 11: Mod loading
    // TODO(Phase 2): instantiate ModLoader(SDL3Filesystem, *rawLogger) when SDL3Filesystem ships.
    // Mods are scanned from PathDomain::Assets/"mods" (install-time directory, not UserData).
    crashReporter.setMods(nullptr, 0);

    // Step 12: Game loop placeholder
    rawLogger->log(LogLevel::Info, __FILE__, __LINE__,
                   "fighters-legacy " FL_VERSION_STRING " (" FL_GIT_HASH ") — Phase 1 stub, no game loop");

    // Step 13: Clean shutdown
    p.renderer->shutdown();
    p.window->shutdown();
    crashReporter.shutdown();
    return 0;
}
