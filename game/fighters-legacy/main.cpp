// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientBackends.h"
#include "Game.h"
#include "Version.h"
#include "gui/ImGuiGui.h"             // #156: Dear ImGui backend behind the IGui HAL
#include "openal/OALAudioFactory.h"   // the OpenAL Soft IAudio backend
#include "vulkan/VkRendererFactory.h" // the Vulkan IRenderer backend
#include <cstdio>
#include <cstring>

using namespace fl;

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("fighters-legacy %s (%s)\n", FL_VERSION_STRING, FL_GIT_HASH);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "Usage: fighters-legacy [options]\n"
                "\n"
                "  --connect <host[:port]>     Connect to a remote fl-server (default port: 4778).\n"
                "                              Omit to start a local single-player session.\n"
                "  --mission <id>              Skip the menu and launch straight into a single-player\n"
                "                              session with this mission (e.g. builtin:sandbox,\n"
                "                              builtin:shape-gallery, or a pack mission id).\n"
                "  --auto                      Skip the menu and enter the session the other flags\n"
                "                              describe: Free Flight, or Join Server with --connect.\n"
                "  --aircraft <type-id>        Request a specific aircraft type (server may clamp).\n"
                "  --observer                  Join as a spectator with no aircraft.\n"
                "  --headless                  Render with no window/display (swapchain-free; pair with\n"
                "                              lavapipe for no-GPU rendering). See docs/developer/demo-recording.md.\n"
                "  --screenshot <path>         Write one PNG a few seconds into Flight, then exit.\n"
                "  --screenshot-frames <n>     Flight frames to wait before the screenshot (default 600).\n"
                "  --frame-stats-json <path>   Record per-frame render timing + VRAM to a JSON report\n"
                "                              (see tools/gpu_contention/ and "
                "docs/developer/decisions/ai-provider-evaluation.md).\n"
                "  --run-seconds <n>           Leave the session n seconds after Flight starts (unattended\n"
                "                              measurement runs; 0 = never, the default).\n"
                "  --record <out.mp4>          Record the mission's cinematic cameras to video (#909).\n"
                "  --record-fps <n>            Recording frame rate (default 30).\n"
                "  --record-res <WxH>          Recording resolution (default 1280x720).\n"
                "  --record-png-dir <dir>      Record to a PNG sequence instead of mp4 (no ffmpeg).\n"
                "  --shot-track <yaml>         Camera shot list (a cameras: doc); defaults to --mission.\n"
                "  --exit-on-mission-end       Stop recording when the mission objective ends.\n"
                "  --record-max-sec <n>        Wall-clock recording cap. Tripping it FAILS the run: the\n"
                "                              shot list is sim time and this is wall clock, and\n"
                "                              fl-server --time-rate is the ratio between them.\n"
                "  --record-max-dup <n>        Fail (non-zero exit) if duplicated frames exceed this.\n"
                "  --assets <dir>              Content root override (also FL_ASSETS_ROOT env var).\n"
                "  --operator-password <pw>    Operator password for admin console commands on the\n"
                "                              remote server. Also read from FL_OPERATOR_PASSWORD env\n"
                "                              var or [client].operator_password in user.toml.\n"
                "  --log-level <level>         Log verbosity: trace|debug|info|warn|error\n"
                "  --version                   Print version string and exit.\n"
                "  --help                      Print this message and exit.\n");
            return 0;
        }
    }
    // The composition root: the only place in the client that names a render, GUI or audio backend
    // (#1067). Everything else lives in game-client, which links none of them — see ClientBackends.h.
    Game game(ClientBackends{
        .createRenderer = [] { return createVulkanRenderer(); },
        .createGui = [](IWindow& window, IRenderer& renderer) { return createImGuiGui(window, renderer); },
        .createAudio = [] { return createOALAudio(); },
    });
    if (!game.init(argc, argv))
        return 1;
    game.run();
    return game.exitCode(); // non-zero when the recorder failed (dup cap / encoder error), else 0
}
