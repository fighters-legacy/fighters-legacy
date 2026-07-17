// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IDisplay.h"
#include "IWindowEventHandler.h"

#include <optional>
#include <string>

namespace fl {

// Threading: all methods must be called from the main thread.
class IWindow {
  public:
    virtual ~IWindow() = default;

    virtual bool init(const char* title, int width, int height) = 0;
    virtual void shutdown() = 0;

    // Called once per frame; backend dispatches all pending OS events to the handler.
    virtual void pollEvents() = 0;

    virtual void setEventHandler(IWindowEventHandler* handler) = 0;

    // Physical framebuffer pixels — GPU/swapchain resolution. On non-HiDPI displays equals
    // logicalWidth()/logicalHeight(); on Retina/HiDPI displays is 2× or more.
    virtual int width() const = 0;
    virtual int height() const = 0;

    // Logical (DPI-independent) window dimensions — matches SDL pointer-event coordinates.
    // Use these for mouse/cursor mapping and UI layout in screen space.
    virtual int logicalWidth() const = 0;
    virtual int logicalHeight() const = 0;

    virtual bool shouldClose() const = 0;

    // Returns the platform-native window handle (HWND / ANativeWindow / NSWindow)
    // as an opaque pointer so the Vulkan backend can create VkSurfaceKHR without
    // any platform header appearing in this file.
    virtual void* nativeHandle() const = 0;

    // Returns a human-readable description of the last error, or nullptr if none.
    // Valid until the next call on this interface.
    virtual const char* getLastError() const = 0;

    enum class MessageBoxType { Info, Warning, Error };

    struct MessageBoxButton {
        int id;
        const char* text;
    };

    // Shows an OS-native modal dialog. Returns the id of the clicked button, or -1
    // on error/dismiss. Safe to call before init() — SDL_ShowMessageBox does not
    // require SDL_Init(VIDEO).
    virtual int showMessageBox(MessageBoxType type, const char* title, const char* message,
                               const MessageBoxButton* buttons, int numButtons) = 0;

    // Opens a URL or file:// URI in the OS default handler.
    virtual void openURL(const char* url) = 0;

    // Blocks until the user picks a folder or cancels, then returns the absolute path of the chosen
    // folder, or nullopt on cancel or error. Main thread only; call after init(). `defaultLocation`
    // may be nullptr (backend default). The default implementation returns nullopt so backends and
    // test doubles without a native picker keep compiling — callers must already handle nullopt
    // (cancel), so "no picker available" degrades to the same path. (#665)
    //
    // ABI note: this virtual is part of the IWindow vtable — the engine and any native plugin must
    // be rebuilt against the same header revision.
    virtual std::optional<std::string> showFolderDialog(const char* title, const char* defaultLocation) {
        (void)title;
        (void)defaultLocation;
        return std::nullopt;
    }

    // Updates the window title bar.
    virtual void setTitle(const char* title) = 0;

    // Resizes the window to the given logical dimensions (windowed mode only).
    // Has no effect in fullscreen. Returns true on success.
    virtual bool setSize(int width, int height) = 0;

    // Enters or exits fullscreen mode using the mode last set by setDisplayMode
    // (or the desktop mode if setDisplayMode was never called). Returns true on success.
    virtual bool setFullscreen(bool fullscreen) = 0;

    // Sets the display mode to apply when in fullscreen. No visible effect while
    // windowed. The mode must come from IDisplay::listModes(). Returns true on success.
    virtual bool setDisplayMode(const IDisplay::DisplayMode& mode) = 0;

    // Returns the 0-based monitor index (matching IDisplay::getMonitorCount() range)
    // that currently contains most of this window. Returns -1 on error or before init().
    virtual int getCurrentMonitorId() const = 0;
};

} // namespace fl
