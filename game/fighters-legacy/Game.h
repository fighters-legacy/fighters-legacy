// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IScreen.h"
#include <memory>
#include <string>

namespace fl {

struct GameImpl;

class Game {
  public:
    Game();
    ~Game();
    bool init(int argc, char** argv);
    void run();

    // Process exit code (#916): non-zero when the cinematic recorder failed (encoder error or the
    // duplicated-frame cap was exceeded — bad video is loud, never silently shipped). 0 otherwise.
    [[nodiscard]] int exitCode() const;

  private:
    bool initPlatform(int argc, char** argv);
    bool initWindowAndRenderer();
    bool initContent();
    bool initRecorder(); // build the ShotDirector + open the encoder + install the capture sink (#916)
    void initGameSystems();
    void buildManualFor(uint32_t typeIndex); // generate the in-flight aircraft manual (#821)
    void initScreenManager();
    void initGameConsole();

    void startGame(const std::string& mission = "");
    void stopGame();
    void handleTransition(Screen next);

    // Cinematic recorder loop hooks (#916). driveRecorderCamera sets the camera pose from the mission's
    // shots via ShotDirector before the view() call; recorderEmit pushes frames to the encoder at
    // capture boundaries and evaluates the stop conditions; recorderFinish closes the encoder and sets
    // the process exit code from the duplicated-frame cap.
    void driveRecorderCamera();
    void recorderEmit(bool& running);
    void recorderFinish();

    // Per-frame joystick reconcile (#1061): refresh the GUID -> index map and this frame's hat state,
    // log devices arriving and leaving, and report bindings whose device is absent. Called right after
    // pollEvents(), before anything reads a binding.
    void reconcileInputDevices();

    std::unique_ptr<GameImpl> m_impl;
};

} // namespace fl
