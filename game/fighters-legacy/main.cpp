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
            std::printf("Usage: fighters-legacy [--log-level trace|debug|info|warn|error] [--version] [--help]\n");
            return 0;
        }
    }
    Game game;
    if (!game.init(argc, argv))
        return 1;
    game.run();
    return 0;
}
