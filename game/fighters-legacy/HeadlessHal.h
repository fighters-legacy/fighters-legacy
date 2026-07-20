// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal no-op IWindow / IDisplay for the swapchain-free headless client mode (#913). The recorder
// (#916) runs with no display and no GPU (lavapipe): there is no OS window, input is not polled (the
// camera is driven by the ShotDirector, not a human), and the renderer presents to owned images via
// IRenderer::initHeadless rather than a swapchain. These keep every `window->`/`display->` call site in
// Game.cpp unchanged so the headless branch is confined to construction.

#include "IDisplay.h"
#include "IWindow.h"

#include <vector>

namespace fl {

// A windowless IWindow: a fixed framebuffer size, no events, no native handle (the renderer uses
// initHeadless, so nativeHandle() is never dereferenced for surface creation).
class HeadlessWindow : public IWindow {
  public:
    HeadlessWindow(int width, int height) : m_w(width), m_h(height) {}

    bool init(const char* /*title*/, int width, int height) override {
        if (width > 0)
            m_w = width;
        if (height > 0)
            m_h = height;
        return true;
    }
    void shutdown() override {}
    void pollEvents() override {}
    void setEventHandler(IWindowEventHandler* /*handler*/) override {}
    int width() const override {
        return m_w;
    }
    int height() const override {
        return m_h;
    }
    int logicalWidth() const override {
        return m_w;
    }
    int logicalHeight() const override {
        return m_h;
    }
    bool shouldClose() const override {
        return false;
    } // the record loop decides when to stop
    void* nativeHandle() const override {
        return nullptr;
    }
    const char* getLastError() const override {
        return nullptr;
    }
    int showMessageBox(MessageBoxType, const char*, const char*, const MessageBoxButton*, int) override {
        return -1;
    }
    void openURL(const char*) override {}
    void setTitle(const char*) override {}
    bool setSize(int width, int height) override {
        m_w = width;
        m_h = height;
        return true;
    }
    bool setFullscreen(bool) override {
        return false;
    }
    bool setDisplayMode(const IDisplay::DisplayMode&) override {
        return false;
    }
    int getCurrentMonitorId() const override {
        return 0;
    }

  private:
    int m_w;
    int m_h;
};

// A no-op IDisplay: one nominal monitor, no fullscreen modes (Settings is unreachable headless).
class HeadlessDisplay : public IDisplay {
  public:
    int getMonitorCount() const override {
        return 1;
    }
    const char* getMonitorName(int) const override {
        return "headless";
    }
    std::vector<DisplayMode> listModes(int) const override {
        return {};
    }
    float getRefreshRate(int) const override {
        return 60.0f;
    }
    const char* getLastError() const override {
        return nullptr;
    }
};

} // namespace fl
