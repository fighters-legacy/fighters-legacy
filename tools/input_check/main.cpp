// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "IWindowEventHandler.h"
#include "Platform.h"
#include "SDL3Input.h"
#include "SDL3Window.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>

static std::atomic<bool> g_quit{false};
static void onSignal(int) {
    g_quit = true;
}

// ASCII bar graph: maps [-1,1] to a 20-char bar.
static void printBar(float v) {
    constexpr int kWidth = 20;
    int filled = static_cast<int>((v + 1.0f) * 0.5f * kWidth + 0.5f);
    filled = filled < 0 ? 0 : (filled > kWidth ? kWidth : filled);
    std::printf("[");
    for (int i = 0; i < kWidth; ++i)
        std::putchar(i < filled ? '=' : ' ');
    std::printf("] %+.2f", static_cast<double>(v));
}

static const char* buttonLabel(GamepadButton b) {
    switch (b) {
    case GamepadButton::A:
        return "A";
    case GamepadButton::B:
        return "B";
    case GamepadButton::X:
        return "X";
    case GamepadButton::Y:
        return "Y";
    case GamepadButton::LeftShoulder:
        return "LB";
    case GamepadButton::RightShoulder:
        return "RB";
    case GamepadButton::LeftTrigger:
        return "LT";
    case GamepadButton::RightTrigger:
        return "RT";
    case GamepadButton::LeftStick:
        return "LS";
    case GamepadButton::RightStick:
        return "RS";
    case GamepadButton::DpadUp:
        return "^";
    case GamepadButton::DpadDown:
        return "v";
    case GamepadButton::DpadLeft:
        return "<";
    case GamepadButton::DpadRight:
        return ">";
    case GamepadButton::Start:
        return "ST";
    case GamepadButton::Back:
        return "BK";
    default:
        return "?";
    }
}

static const char* axisLabel(GamepadAxis a) {
    switch (a) {
    case GamepadAxis::LeftX:
        return "LX";
    case GamepadAxis::LeftY:
        return "LY";
    case GamepadAxis::RightX:
        return "RX";
    case GamepadAxis::RightY:
        return "RY";
    case GamepadAxis::TriggerLeft:
        return "TL";
    case GamepadAxis::TriggerRight:
        return "TR";
    default:
        return "??";
    }
}

class App : public IWindowEventHandler {
  public:
    explicit App(SDL3Window& window, SDL3Input& input) : m_window(window), m_input(input) {}

    void onResize(int, int) override {}
    void onClose() override {
        m_running = false;
    }

    int run() {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!m_window.init("Fighters Legacy — Input Test", 640, 120)) {
            std::fprintf(stderr, "window init failed: %s\n", m_window.getLastError());
            return 1;
        }
        m_window.setEventHandler(this);
        m_window.setInputSink(&m_input);

        while (m_running && !m_window.shouldClose() && !g_quit) {
            m_window.pollEvents();
            render();
            m_input.flush();
            SDL_Delay(16); // ~60 Hz
        }

        m_window.shutdown();
        return 0;
    }

  private:
    void render() {
        // Check for rumble triggers before clearing screen
        if (m_input.getGamepadCount() > 0) {
            if (m_input.isGamepadButtonJustPressed(0, GamepadButton::A))
                m_input.rumble(0, 0.5f, 0.5f, 300);
            if (m_input.isGamepadButtonJustPressed(0, GamepadButton::B))
                m_input.rumbleTriggers(0, 0.8f, 0.8f, 300);
            if (m_input.isGamepadButtonJustPressed(0, GamepadButton::X))
                m_input.stopRumble(0);
        }

        // Check exit
        if (m_input.isKeyJustPressed(Key::Escape))
            m_running = false;

        // Clear terminal and print state
        std::printf("\033[2J\033[H");
        std::printf("=== Fighters Legacy — Input Test ===\n");
        std::printf("Esc=quit  A=body rumble  B=trigger rumble  X=stop rumble\n\n");

        int gpCount = m_input.getGamepadCount();
        if (gpCount == 0) {
            std::printf("No gamepads connected.\n");
        }
        for (int gp = 0; gp < gpCount; ++gp) {
            // SDL_GetGamepadName requires the SDL_Gamepad* handle; we can query it
            // via the SDL API directly using the instance IDs. For the tool we just
            // print the index.
            std::printf("--- Gamepad %d  rumble:%s  trigger-rumble:%s ---\n", gp,
                        m_input.supportsRumble(gp) ? "yes" : "no", m_input.supportsTriggerRumble(gp) ? "yes" : "no");

            // Buttons
            std::printf("Buttons: ");
            for (int b = 0; b < static_cast<int>(GamepadButton::Count); ++b) {
                bool pressed = m_input.isGamepadButtonDown(gp, static_cast<GamepadButton>(b));
                std::printf("%s[%s] ", buttonLabel(static_cast<GamepadButton>(b)), pressed ? "X" : " ");
            }
            std::printf("\n");

            // Axes
            for (int a = 0; a < static_cast<int>(GamepadAxis::Count); ++a) {
                float v = m_input.getGamepadAxis(gp, static_cast<GamepadAxis>(a));
                std::printf("%s ", axisLabel(static_cast<GamepadAxis>(a)));
                printBar(v);
                std::printf("\n");
            }
        }

        // Mouse
        int mx, my, dx, dy;
        m_input.getMousePosition(mx, my);
        m_input.getMouseDelta(dx, dy);
        std::printf("\nMouse: (%d,%d)  delta:(%+d,%+d)  scroll:%d  ", mx, my, dx, dy, m_input.getMouseScroll());
        std::printf("LMB[%s] MMB[%s] RMB[%s]\n", m_input.isMouseButtonDown(MouseButton::Left) ? "X" : " ",
                    m_input.isMouseButtonDown(MouseButton::Middle) ? "X" : " ",
                    m_input.isMouseButtonDown(MouseButton::Right) ? "X" : " ");

        std::fflush(stdout);
    }

    SDL3Window& m_window;
    SDL3Input& m_input;
    bool m_running{true};
};

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SDL3Window window;
    SDL3Input input;
    return App(window, input).run();
}
