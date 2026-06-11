// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IScreen.h"
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
    void initScreenManager();
    void initGameConsole();

    void startGame();
    void stopGame();
    void handleTransition(Screen next);

    std::unique_ptr<GameImpl> m_impl;
};
