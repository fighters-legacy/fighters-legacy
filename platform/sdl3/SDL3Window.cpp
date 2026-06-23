// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "SDL3Window.h"
#include "IWindowEventHandler.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <vector>

namespace fl {

bool SDL3Window::init(const char* title, int width, int height) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        m_lastError = SDL_GetError();
        return false;
    }

    m_window = SDL_CreateWindow(title, width, height,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window) {
        m_lastError = SDL_GetError();
        SDL_Quit();
        return false;
    }

    if (!SDL_GetWindowSizeInPixels(m_window, &m_width, &m_height)) {
        m_width = width;
        m_height = height;
    }
    if (!SDL_GetWindowSize(m_window, &m_logicalWidth, &m_logicalHeight)) {
        m_logicalWidth = width;
        m_logicalHeight = height;
    }
    return true;
}

void SDL3Window::shutdown() {
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
}

void SDL3Window::pollEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (m_inputSink)
            m_inputSink->onSDLEvent(ev);
        if (m_joystickSink)
            m_joystickSink->onSDLEvent(ev);
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            m_shouldClose = true;
            if (m_handler)
                m_handler->onClose();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_shouldClose = true;
            if (m_handler)
                m_handler->onClose();
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            m_width = ev.window.data1;
            m_height = ev.window.data2;
            if (m_handler)
                m_handler->onResize(m_width, m_height);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            m_logicalWidth = ev.window.data1;
            m_logicalHeight = ev.window.data2;
            break;
        default:
            break;
        }
    }
}

void SDL3Window::setEventHandler(IWindowEventHandler* handler) {
    m_handler = handler;
}

int SDL3Window::width() const {
    return m_width;
}

int SDL3Window::height() const {
    return m_height;
}

int SDL3Window::logicalWidth() const {
    return m_logicalWidth;
}

int SDL3Window::logicalHeight() const {
    return m_logicalHeight;
}

bool SDL3Window::shouldClose() const {
    return m_shouldClose;
}

void* SDL3Window::nativeHandle() const {
    return m_window;
}

const char* SDL3Window::getLastError() const {
    m_lastError = SDL_GetError();
    return m_lastError.c_str();
}

int SDL3Window::showMessageBox(MessageBoxType type, const char* title, const char* message,
                               const MessageBoxButton* buttons, int numButtons) {
    std::vector<SDL_MessageBoxButtonData> sdlBtns(static_cast<std::size_t>(numButtons));
    for (int i = 0; i < numButtons; ++i) {
        sdlBtns[static_cast<std::size_t>(i)] = {0, buttons[i].id, buttons[i].text};
    }
    if (numButtons > 0)
        sdlBtns[0].flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    if (numButtons > 1)
        sdlBtns[static_cast<std::size_t>(numButtons - 1)].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;

    Uint32 flags = (type == MessageBoxType::Error)     ? SDL_MESSAGEBOX_ERROR
                   : (type == MessageBoxType::Warning) ? SDL_MESSAGEBOX_WARNING
                                                       : SDL_MESSAGEBOX_INFORMATION;
    SDL_MessageBoxData data{flags, nullptr, title, message, numButtons, sdlBtns.data(), nullptr};
    int clicked = -1;
    SDL_ShowMessageBox(&data, &clicked);
    return clicked;
}

void SDL3Window::openURL(const char* url) {
    SDL_OpenURL(url);
}

void SDL3Window::setTitle(const char* title) {
    if (m_window)
        SDL_SetWindowTitle(m_window, title);
}

bool SDL3Window::setSize(int w, int h) {
    if (!m_window)
        return false;
    if (!SDL_SetWindowSize(m_window, w, h)) {
        m_lastError = SDL_GetError();
        return false;
    }
    return true;
}

bool SDL3Window::setFullscreen(bool fullscreen) {
    if (!m_window)
        return false;
    if (!SDL_SetWindowFullscreen(m_window, fullscreen)) {
        m_lastError = SDL_GetError();
        return false;
    }
    return true;
}

bool SDL3Window::setDisplayMode(const IDisplay::DisplayMode& mode) {
    if (!m_window)
        return false;
    m_pendingMode = mode;

    SDL_DisplayID displayId = SDL_GetDisplayForWindow(m_window);
    if (!displayId) {
        m_lastError = SDL_GetError();
        return false;
    }

    int count = 0;
    auto modes = SDL_GetFullscreenDisplayModes(displayId, &count);
    if (!modes) {
        m_lastError = SDL_GetError();
        return false;
    }

    constexpr float kRefreshEpsilon = 0.5f;
    for (int i = 0; i < count; ++i) {
        if (modes[i]->w == mode.width && modes[i]->h == mode.height &&
            std::abs(modes[i]->refresh_rate - mode.refreshRate) < kRefreshEpsilon) {
            if (!SDL_SetWindowFullscreenMode(m_window, modes[i])) {
                m_lastError = SDL_GetError();
                return false;
            }
            return true;
        }
    }

    m_lastError = "no matching display mode found";
    return false;
}

int SDL3Window::getCurrentMonitorId() const {
    if (!m_window)
        return -1;
    SDL_DisplayID id = SDL_GetDisplayForWindow(m_window);
    if (!id)
        return -1;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays)
        return -1;
    int result = -1;
    for (int i = 0; i < count; ++i) {
        if (displays[i] == id) {
            result = i;
            break;
        }
    }
    SDL_free(displays);
    return result;
}

} // namespace fl
