// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

namespace fl {

// Threading: all methods must be called from the main thread.
// IDisplay is usable only after IWindow::init() has been called
// (which initializes SDL_INIT_VIDEO). No init()/shutdown() lifecycle.
class IDisplay {
  public:
    virtual ~IDisplay() = default;

    struct DisplayMode {
        int monitorId; // 0-based index; matches getMonitorCount() range
        int width;
        int height;
        float refreshRate; // Hz; 0.0f if unknown
    };

    // Returns the number of connected monitors, or 0 on failure.
    virtual int getMonitorCount() const = 0;

    // Returns a human-readable monitor name for the given 0-based index,
    // or nullptr on error or out-of-range. Valid until the next call on this interface.
    virtual const char* getMonitorName(int monitorId) const = 0;

    // Returns all supported fullscreen display modes for the given monitor.
    // Returns an empty vector on failure or out-of-range monitorId.
    virtual std::vector<DisplayMode> listModes(int monitorId) const = 0;

    // Returns the refresh rate of the given monitor in Hz, or 0.0f on failure.
    // Use IWindow::getCurrentMonitorId() to obtain the window's current monitor.
    virtual float getRefreshRate(int monitorId) const = 0;

    // Returns a human-readable description of the last error, or nullptr if none.
    // Valid until the next call on this interface.
    virtual const char* getLastError() const = 0;
};

} // namespace fl
