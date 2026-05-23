// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Threading: callbacks are invoked from the main thread inside IWindow::pollEvents().
class IWindowEventHandler {
  public:
    virtual ~IWindowEventHandler() = default;

    virtual void onResize(int width, int height) = 0;
    virtual void onClose() = 0;
};
