// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IWindow.h"
#include <string>

struct SDL_Window;

class SDL3Window : public IWindow {
  public:
    bool init(const char* title, int width, int height) override;
    void shutdown() override;
    void pollEvents() override;
    void setEventHandler(IWindowEventHandler* handler) override;
    int width() const override;
    int height() const override;
    bool shouldClose() const override;
    void* nativeHandle() const override;
    const char* getLastError() const override;

  private:
    SDL_Window* m_window{nullptr};
    IWindowEventHandler* m_handler{nullptr};
    int m_width{0};
    int m_height{0};
    bool m_shouldClose{false};
    mutable std::string m_lastError;
};
