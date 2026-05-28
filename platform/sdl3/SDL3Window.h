// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IWindow.h"
#include "SDL3EventSink.h"
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
    int showMessageBox(MessageBoxType type, const char* title, const char* message, const MessageBoxButton* buttons,
                       int numButtons) override;
    void openURL(const char* url) override;
    void setTitle(const char* title) override;
    bool setFullscreen(bool fullscreen) override;
    bool setDisplayMode(const IDisplay::DisplayMode& mode) override;
    int getCurrentMonitorId() const override;

    // Wire up the input backend to receive SDL events during pollEvents().
    void setInputSink(ISDL3EventSink* sink) {
        m_inputSink = sink;
    }

  private:
    SDL_Window* m_window{nullptr};
    IWindowEventHandler* m_handler{nullptr};
    ISDL3EventSink* m_inputSink{nullptr};
    int m_width{0};
    int m_height{0};
    bool m_shouldClose{false};
    IDisplay::DisplayMode m_pendingMode{};
    mutable std::string m_lastError;
};
