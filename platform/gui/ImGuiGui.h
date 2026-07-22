// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IGui.h"

#include <memory>
#include <string>

namespace fl {

class IWindow;
class IRenderer;

// The Dear ImGui reference backend for the IGui HAL (#156, from epic #593). It owns the ImGui context
// and the SDL3 platform backend; the ImGui *Vulkan* renderer backend is driven through
// IRenderer::initGuiRenderBackend() (only VkRenderer holds the Vulkan handles). This header is
// deliberately ImGui-free (no ImGui/SDL/Vulkan types) so the game includes it directly and stays
// backend-agnostic — the same discipline as VkRendererFactory.h.
class ImGuiGui final : public IGui {
  public:
    ImGuiGui(IWindow& window, IRenderer& renderer);
    ~ImGuiGui() override;

    ImGuiGui(const ImGuiGui&) = delete;
    ImGuiGui& operator=(const ImGuiGui&) = delete;

    [[nodiscard]] bool isValid() const noexcept {
        return m_valid;
    }

    void newFrame() override;
    void render() override;
    void processEvent(const void* nativeEvent) override;

    bool beginWindow(std::string_view title, float xN, float yN, float wN, float hN) override;
    void endWindow() override;
    void label(std::string_view text) override;
    void separator() override;
    void sameLine() override;
    bool inputText(std::string_view label, char* buf, std::size_t cap, bool masked = false) override;
    bool button(std::string_view label) override;
    bool selectable(std::string_view label, bool selected) override;
    bool beginTable(std::string_view id, int columns) override;
    void tableHeadersRow(std::span<const std::string_view> headers) override;
    void tableNextRow() override;
    void tableCell(std::string_view text) override;
    void endTable() override;
    [[nodiscard]] bool wantCaptureKeyboard() const override;
    [[nodiscard]] bool wantCaptureMouse() const override;

  private:
    // Null-terminate a string_view into a reused scratch buffer for ImGui's char*-taking APIs. The
    // pointer is valid only until the next call, which is all ImGui needs (it copies at the call site).
    const char* z(std::string_view s);

    IRenderer& m_renderer;
    std::string m_scratch;
    bool m_valid{false};       // ImGui context + SDL backend up
    bool m_vulkanReady{false}; // renderer's ImGui Vulkan backend up (draw data will be recorded)
    bool m_frameActive{false}; // a newFrame() is open (guards render()/endWindow pairing)
};

// Factory (mirrors createVulkanRenderer): build an ImGuiGui bound to the window + renderer. Returns a
// valid IGui, or nullptr if the ImGui Vulkan backend could not initialise (the caller then runs with no
// IGui, i.e. the HudElement-only UI). The game owns the returned object.
std::unique_ptr<IGui> createImGuiGui(IWindow& window, IRenderer& renderer);

} // namespace fl
