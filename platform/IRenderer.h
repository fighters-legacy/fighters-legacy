// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

namespace fl {

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

    // 2D texture-ARRAY upload (#446) — a KTX2 with numLayers > 1, or the raw-RGBA path when
    // TextureUploadDesc::rawLayers > 1 (layer-major). Used by the terrain biome arrays. Non-pure with
    // an invalid-handle default so a backend/mock without array support (and every test mock) needs no
    // change; VkRenderer overrides it.
    virtual TextureHandle createTextureArray(const TextureUploadDesc& desc) {
        (void)desc;
        return {};
    }

    // Bind the terrain biome texture arrays (#446): base color + combined normal/roughness, both 2D
    // arrays with `layerCount` layers (layer index = biome id). Invalid handles unbind (fall back to
    // the builtin). Non-pure no-op default (mocks/headless need no change).
    virtual void setTerrainBiomeTextures(TextureHandle colorArray, TextureHandle normalOrmArray, uint32_t layerCount) {
        (void)colorArray;
        (void)normalOrmArray;
        (void)layerCount;
    }

    // Create a PBR material linking already-uploaded textures.
    virtual MaterialHandle createMaterial(const MaterialDesc& desc) = 0;

    // The PBR material createMesh built from the mesh's own glTF material (base color / normal / ORM),
    // when a MeshUploadDesc::textureResolver was supplied (#833). Invalid handle when the mesh had no
    // material or no resolver — the caller then falls back to a flat colour.
    virtual MaterialHandle getMeshMaterial(MeshHandle h) const = 0;

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

    // ── Per-frame stats ─────────────────────────────────────────────────────
    // Returns statistics from the most recently completed frame.
    // Safe to call at any time; values are zero until at least one frame completes.
    virtual FrameStats getFrameStats() const = 0;

    // ── Debug overlay ───────────────────────────────────────────────────────
    // Renders text lines as a white monospace overlay in the top-left corner.
    // Must be called between beginFrame and endFrame. Empty span = no overlay.
    // The span and all string_view data must remain alive until endFrame returns.
    virtual void setOverlayLines(std::span<const std::string_view> lines) = 0;

    // ── 2D game overlay ──────────────────────────────────────────────────────
    // Appends 2D overlay elements (cockpit HUD, rain, notices, etc.) to this
    // frame's list. May be called multiple times per frame. Copies elements into
    // renderer-owned storage; caller lifetime is irrelevant. Cleared by endFrame.
    virtual void submitOverlayElements(std::span<const HudElement> elements) = 0;

    // ── Game console overlay ──────────────────────────────────────────────────
    // Sets the game console overlay for this frame (engine-level command shell).
    // Non-owning view; the span must remain valid until endFrame returns.
    // Cleared by endFrame.
    virtual void setConsoleElements(std::span<const HudElement> elements) = 0;

    // Capture the next presented frame to a PNG at `path` (#909 groundwork). Returns true if the
    // request was accepted (the write happens at the end of the current/next frame). Non-pure with a
    // false default so a backend/mock without capture support (and every test mock) needs no change.
    virtual bool captureScreenshot(const char* path) {
        (void)path;
        return false;
    }
};

} // namespace fl
