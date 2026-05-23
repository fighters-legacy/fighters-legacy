// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "SDL3Window.h"
#include "IWindowEventHandler.h"
#include <SDL3/SDL.h>

bool SDL3Window::init(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
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
