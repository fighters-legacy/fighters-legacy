// SPDX-License-Identifier: GPL-3.0-or-later
#include "Game.h"
#include "Version.h"
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
            std::printf("Usage: fighters-legacy [options]\n"
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
                        "                              lavapipe for no-GPU rendering). See docs/demo-recording.md.\n"
                        "  --screenshot <path>         Write one PNG a few seconds into Flight, then exit.\n"
                        "  --record <out.mp4>          Record the mission's cinematic cameras to video (#909).\n"
                        "  --record-fps <n>            Recording frame rate (default 30).\n"
                        "  --record-res <WxH>          Recording resolution (default 1280x720).\n"
                        "  --record-png-dir <dir>      Record to a PNG sequence instead of mp4 (no ffmpeg).\n"
                        "  --shot-track <yaml>         Camera shot list (a cameras: doc); defaults to --mission.\n"
                        "  --exit-on-mission-end       Stop recording when the mission objective ends.\n"
                        "  --record-max-sec <n>        Wall-clock recording cap (safety stop).\n"
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
    Game game;
    if (!game.init(argc, argv))
        return 1;
    game.run();
    return game.exitCode(); // non-zero when the recorder failed (dup cap / encoder error), else 0
}
