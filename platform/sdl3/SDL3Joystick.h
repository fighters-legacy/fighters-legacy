// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IJoystick.h"
#include "SDL3EventSink.h"
#include <string>
#include <vector>

struct SDL_Joystick;
typedef uint32_t SDL_JoystickID;

namespace fl {

class SDL3Joystick : public IJoystick, public ISDL3EventSink {
  public:
    // --- IJoystick ---
    int getJoystickCount() const override;
    const char* getJoystickName(int joystickId) const override;
    const char* getJoystickGuid(int joystickId) const override;
    int getAxisCount(int joystickId) const override;
    float getAxisValue(int joystickId, int axisIndex) const override;
    int getHatCount(int joystickId) const override;
    HatPosition getHatPosition(int joystickId, int hatIndex) const override;
    int getButtonCount(int joystickId) const override;
    bool isButtonDown(int joystickId, int buttonIndex) const override;
    bool isButtonJustPressed(int joystickId, int buttonIndex) const override;
    void setEventHandler(IJoystickEventHandler* handler) override;
    void flush() override;
    const char* getLastError() const override;

    // --- ISDL3EventSink ---
    void onSDLEvent(const SDL_Event& ev) override;

  private:
    struct JoystickState {
        SDL_JoystickID sdlId{0};
        SDL_Joystick* handle{nullptr};
        std::string name;
        std::string guid;
        std::vector<float> axes;
        std::vector<HatPosition> hats;
        std::vector<bool> buttons;
        std::vector<bool> justPressed;
    };

    std::vector<JoystickState> m_joysticks;
    IJoystickEventHandler* m_eventHandler{nullptr};
    std::string m_lastError;

    int findJoystick(SDL_JoystickID id) const;
    JoystickState* joystickAt(int joystickId);
    const JoystickState* joystickAt(int joystickId) const;
};

} // namespace fl
