// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

enum class CursorShape : uint8_t {
    Arrow,
    Hand,
    Crosshair,
    ResizeNS,
    ResizeEW,
    ResizeAll,
    Text,
    None, // hides the cursor; consistent with IInput::setMouseCapture(true) for flight view
};

// Threading: all methods must be called from the main thread.
// ICursor is usable only after IWindow::init() has been called (requires SDL_INIT_VIDEO).
// No init()/shutdown() lifecycle.
class ICursor {
  public:
    virtual ~ICursor() = default;

    // Switches to a standard OS cursor shape. CursorShape::None hides the cursor.
    virtual void setCursor(CursorShape shape) = 0;

    // Loads and activates a custom RGBA bitmap cursor. pixels must point to
    // width*height*4 bytes of RGBA data. hotX/hotY is the click point within the bitmap.
    virtual void setCustomCursor(const void* pixels, int width, int height, int hotX, int hotY) = 0;

    // Returns a human-readable description of the last error, or nullptr if none.
    virtual const char* getLastError() const = 0;
};

} // namespace fl
