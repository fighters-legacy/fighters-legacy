// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3Display.h"
#include <SDL3/SDL.h>
#include <memory>

uint32_t SDL3Display::displayIdForMonitor(int monitorId) const {
    int count = 0;
    auto displays = std::unique_ptr<SDL_DisplayID[], decltype(&SDL_free)>(SDL_GetDisplays(&count), SDL_free);
    if (!displays || monitorId < 0 || monitorId >= count)
        return 0;
    return displays[monitorId];
}

int SDL3Display::getMonitorCount() const {
    int count = 0;
    auto displays = std::unique_ptr<SDL_DisplayID[], decltype(&SDL_free)>(SDL_GetDisplays(&count), SDL_free);
    if (!displays) {
        m_lastError = SDL_GetError();
        return 0;
    }
    return count;
}

const char* SDL3Display::getMonitorName(int monitorId) const {
    SDL_DisplayID id = displayIdForMonitor(monitorId);
    if (!id)
        return nullptr;
    const char* name = SDL_GetDisplayName(id);
    if (!name) {
        m_lastError = SDL_GetError();
        return nullptr;
    }
    m_nameBuffer = name;
    return m_nameBuffer.c_str();
}

std::vector<IDisplay::DisplayMode> SDL3Display::listModes(int monitorId) const {
    SDL_DisplayID id = displayIdForMonitor(monitorId);
    if (!id)
        return {};
    int count = 0;
    auto modes = SDL_GetFullscreenDisplayModes(id, &count);
    if (!modes) {
        m_lastError = SDL_GetError();
        return {};
    }
    std::vector<DisplayMode> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({monitorId, modes[i]->w, modes[i]->h, modes[i]->refresh_rate});
    return result;
}

float SDL3Display::getRefreshRate(int monitorId) const {
    SDL_DisplayID id = displayIdForMonitor(monitorId);
    if (!id)
        return 0.0f;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
    if (!mode) {
        m_lastError = SDL_GetError();
        return 0.0f;
    }
    return mode->refresh_rate;
}

const char* SDL3Display::getLastError() const {
    m_lastError = SDL_GetError();
    return m_lastError.c_str();
}
