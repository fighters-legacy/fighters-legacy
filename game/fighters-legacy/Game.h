// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>

struct GameImpl;

class Game {
  public:
    Game();
    ~Game();
    bool init(int argc, char** argv);
    void run();

  private:
    bool initPlatform(int argc, char** argv);
    bool initWindowAndRenderer();
    bool initContent();
    void initGameSystems();
    bool initNetwork();
    void initDebugConsole();

    std::unique_ptr<GameImpl> m_impl;
};
