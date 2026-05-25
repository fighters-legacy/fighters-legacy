// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IWindowEventHandler.h"

// Threading: all methods must be called from the main thread.
class IWindow {
  public:
    virtual ~IWindow() = default;

    virtual bool init(const char* title, int width, int height) = 0;
    virtual void shutdown() = 0;

    // Called once per frame; backend dispatches all pending OS events to the handler.
    virtual void pollEvents() = 0;

    virtual void setEventHandler(IWindowEventHandler* handler) = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
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
};
