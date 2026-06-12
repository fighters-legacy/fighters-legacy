// SPDX-License-Identifier: GPL-3.0-or-later
#include "Game.h"
#include "Version.h"
#include <cstdio>
#include <cstring>

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
    return 0;
}
