// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

// Forward declaration: IRenderer only holds an IWindow* pointer, so the full
// type definition is not needed here.
class IWindow;

// Retained-resource scene renderer interface.
//
// Resource lifetime (call outside begin/endFrame, main thread only):
//   createMesh / createTexture / createMaterial — upload to GPU, return handle.
//   destroyMesh / destroyTexture / destroyMaterial — deferred-delete (safe to
//     call immediately; GPU cleanup is deferred until in-flight frames complete).
//
// Per-frame submission (between beginFrame and endFrame):
//   setScene — upload camera/light UBO data, store RenderItems for this frame.
//
// Threading: all methods must be called from the main thread.
class IRenderer {
  public:
    virtual ~IRenderer() = default;

    // Uses window->nativeHandle() to create VkSurfaceKHR; must be called after
    // IWindow::init.
    virtual bool init(IWindow* window) = 0;

    // Must be called when the window framebuffer changes size so the renderer
    // can tear down and rebuild the swapchain.
    virtual void onResize(int width, int height) = 0;

    // Acquires the next swapchain image and resets the command buffer.
    virtual void beginFrame() = 0;

    // Records and submits the frame command buffer; presents the swapchain image.
    virtual void endFrame() = 0;

    // Destroys all GPU resources in correct dependency order.
    virtual void shutdown() = 0;

    // Returns a human-readable description of the last error, or nullptr if none.
    virtual const char* getLastError() const = 0;

    // Returns a human-readable GPU + driver string, e.g.
    // "NVIDIA GeForce RTX 3080 (Vulkan driver 456.38.0)". Empty before init().
    virtual const char* gpuInfo() const = 0;

    // ── Resource creation ──────────────────────────────────────────────────
    // Upload a glTF 2.0 mesh (first primitive of the first mesh node).
    virtual MeshHandle createMesh(const MeshUploadDesc& desc) = 0;

    // Upload a texture (KTX2 with Basis Universal transcode, or PNG fallback).
    virtual TextureHandle createTexture(const TextureUploadDesc& desc) = 0;

    // Create a PBR material linking already-uploaded textures.
    virtual MaterialHandle createMaterial(const MaterialDesc& desc) = 0;

    // ── Resource destruction ───────────────────────────────────────────────
    virtual void destroyMesh(MeshHandle h) = 0;
    virtual void destroyTexture(TextureHandle h) = 0;
    virtual void destroyMaterial(MaterialHandle h) = 0;

    // ── Per-frame scene submission ─────────────────────────────────────────
    // Submit the scene for the current frame. Must be called between beginFrame
    // and endFrame. Spans are non-owning; the caller must keep backing arrays
    // alive until after endFrame returns.
    virtual void setScene(const FrameScene& scene) = 0;

    // ── Settings ───────────────────────────────────────────────────────────
    // Apply renderer settings (vsync, FXAA, bloom, etc.).  Safe to call at
    // any time outside of begin/endFrame; changes take effect on the next
    // frame (vsync requires swapchain recreation and is applied on the next
    // resize or explicit recreate).
    virtual void applySettings(const RendererSettings& settings) = 0;
};
