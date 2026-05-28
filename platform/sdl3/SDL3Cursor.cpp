// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "SDL3Cursor.h"
#include <SDL3/SDL.h>

static SDL_SystemCursor toSDLCursor(CursorShape shape) {
    switch (shape) {
    case CursorShape::Arrow:
        return SDL_SYSTEM_CURSOR_DEFAULT;
    case CursorShape::Hand:
        return SDL_SYSTEM_CURSOR_POINTER;
    case CursorShape::Crosshair:
        return SDL_SYSTEM_CURSOR_CROSSHAIR;
    case CursorShape::ResizeNS:
        return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case CursorShape::ResizeEW:
        return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case CursorShape::ResizeAll:
        return SDL_SYSTEM_CURSOR_MOVE;
    case CursorShape::Text:
        return SDL_SYSTEM_CURSOR_TEXT;
    default:
        return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

SDL3Cursor::~SDL3Cursor() {
    releaseCurrentCursor();
    if (m_hidden)
        SDL_ShowCursor();
}

void SDL3Cursor::releaseCurrentCursor() {
    if (m_cursor) {
        SDL_DestroyCursor(m_cursor);
        m_cursor = nullptr;
    }
}

void SDL3Cursor::setCursor(CursorShape shape) {
    if (shape == CursorShape::None) {
        releaseCurrentCursor();
        SDL_HideCursor();
        m_hidden = true;
        return;
    }

    if (m_hidden) {
        SDL_ShowCursor();
        m_hidden = false;
    }

    SDL_Cursor* cursor = SDL_CreateSystemCursor(toSDLCursor(shape));
    if (!cursor) {
        m_lastError = SDL_GetError();
        return;
    }

    releaseCurrentCursor();
    m_cursor = cursor;
    SDL_SetCursor(m_cursor);
}

void SDL3Cursor::setCustomCursor(const void* pixels, int width, int height, int hotX, int hotY) {
    if (!pixels) {
        m_lastError = "pixels must not be null";
        return;
    }

    if (m_hidden) {
        SDL_ShowCursor();
        m_hidden = false;
    }

    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, const_cast<void*>(pixels), width * 4);
    if (!surface) {
        m_lastError = SDL_GetError();
        return;
    }

    SDL_Cursor* cursor = SDL_CreateColorCursor(surface, hotX, hotY);
    SDL_DestroySurface(surface);

    if (!cursor) {
        m_lastError = SDL_GetError();
        return;
    }

    releaseCurrentCursor();
    m_cursor = cursor;
    SDL_SetCursor(m_cursor);
}

const char* SDL3Cursor::getLastError() const {
    m_lastError = SDL_GetError();
    return m_lastError.c_str();
}
