// SPDX-License-Identifier: GPL-3.0-or-later
#include "IWindowEventHandler.h"
#include "Platform.h"
#include "SDL3Window.h"
#include "VkRenderer.h"
#include <cstdio>
#include <memory>

class App : public IWindowEventHandler {
  public:
    explicit App(Platform& p) : m_platform(p) {}

    void onResize(int w, int h) override {
        m_platform.renderer->onResize(w, h);
    }
    void onClose() override {
        m_running = false;
    }

    int run() {
        if (!m_platform.window->init("Fighters Legacy — Hello Triangle", 1280, 720)) {
            std::fprintf(stderr, "window init failed: %s\n", m_platform.window->getLastError());
            return 1;
        }
        m_platform.window->setEventHandler(this);

        if (!m_platform.renderer->init(m_platform.window.get())) {
            std::fprintf(stderr, "renderer init failed: %s\n", m_platform.renderer->getLastError());
            return 1;
        }

        while (m_running && !m_platform.window->shouldClose()) {
            m_platform.window->pollEvents();
            m_platform.renderer->beginFrame();
            m_platform.renderer->endFrame();
        }

        m_platform.renderer->shutdown();
        m_platform.window->shutdown();
        return 0;
    }

  private:
    Platform& m_platform;
    bool m_running{true};
};

int main() {
    Platform p;
    p.window = std::make_unique<SDL3Window>();
    p.renderer = std::make_unique<VkRenderer>();
    return App(p).run();
}
