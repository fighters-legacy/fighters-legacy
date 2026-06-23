// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IDisplay.h"
#include <cstdint>
#include <string>

namespace fl {

class SDL3Display : public IDisplay {
  public:
    int getMonitorCount() const override;
    const char* getMonitorName(int monitorId) const override;
    std::vector<DisplayMode> listModes(int monitorId) const override;
    float getRefreshRate(int monitorId) const override;
    const char* getLastError() const override;

  private:
    // Maps a 0-based monitorId to the SDL_DisplayID at that position.
    // Returns 0 if monitorId is out of range or SDL_GetDisplays fails.
    // SDL_DisplayID is uint32_t; using uint32_t avoids including <SDL3/SDL.h> here.
    uint32_t displayIdForMonitor(int monitorId) const;

    mutable std::string m_lastError;
    mutable std::string m_nameBuffer; // backing storage for getMonitorName return value
};

} // namespace fl
