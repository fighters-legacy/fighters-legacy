// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Forward declaration: IRenderer only holds an IWindow* pointer, so the full
// type definition is not needed here. This keeps compile times down and prevents
// circular include chains.
class IWindow;

// Phase 1 lifecycle interface only. Scene submission (mesh handles, transforms,
// materials, render graph) is added in the Vulkan backend workstream.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Uses window->nativeHandle() to create VkSurfaceKHR; must be called after IWindow::init.
    virtual bool init(IWindow* window) = 0;

    // Must be called when the window framebuffer changes size so the renderer can
    // tear down and rebuild the swapchain. The engine game loop calls this from
    // its IWindowEventHandler::onResize implementation.
    virtual void onResize(int width, int height) = 0;

    // Acquires the next swapchain image and begins command buffer recording.
    virtual void beginFrame() = 0;

    // Submits the recorded command buffer to the GPU queue and presents the
    // swapchain image to the display.
    virtual void endFrame() = 0;

    // Destroys all GPU resources in correct dependency order.
    virtual void shutdown() = 0;

    // Returns a human-readable description of the last error, or nullptr if none.
    // Valid until the next call on this interface.
    virtual const char* getLastError() const = 0;
};
