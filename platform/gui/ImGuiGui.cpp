// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ImGuiGui.h"

#include "IRenderer.h"
#include "IWindow.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h" // ImGui_ImplVulkan_NewFrame (the Vulkan render backend is in VkRenderer)
#include "imgui.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

namespace fl {

namespace {
// Map a normalized [0,1] fraction to a pixel coordinate against the current ImGui display size.
ImVec2 toPixels(float xN, float yN) {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    return ImVec2(xN * ds.x, yN * ds.y);
}
} // namespace

ImGuiGui::ImGuiGui(IWindow& window, IRenderer& renderer) : m_renderer(renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // do not write imgui.ini into the working dir
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();

    auto* sdlWindow = static_cast<SDL_Window*>(window.nativeHandle());
    if (sdlWindow == nullptr || !ImGui_ImplSDL3_InitForVulkan(sdlWindow)) {
        ImGui::DestroyContext();
        return;
    }
    m_valid = true;

    // Bring up the ImGui Vulkan backend inside the renderer (it owns the Vulkan handles). If the renderer
    // is headless / not Vulkan, this is a no-op returning false and we simply never record draw data —
    // the widget vocabulary still runs (useful for tests / a future software backend).
    m_vulkanReady = m_renderer.initGuiRenderBackend();
}

ImGuiGui::~ImGuiGui() {
    if (!m_valid)
        return;
    // Tear the Vulkan backend down BEFORE destroying the context it was initialised against.
    if (m_vulkanReady)
        m_renderer.shutdownGuiRenderBackend();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

const char* ImGuiGui::z(std::string_view s) {
    m_scratch.assign(s.data(), s.size());
    return m_scratch.c_str();
}

void ImGuiGui::processEvent(const void* nativeEvent) {
    if (!m_valid || nativeEvent == nullptr)
        return;
    ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(nativeEvent));
}

void ImGuiGui::newFrame() {
    if (!m_valid)
        return;
    if (m_frameActive)
        ImGui::EndFrame(); // a prior frame was never render()ed (a skipped render) — discard it cleanly
    if (m_vulkanReady)
        ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    m_frameActive = true;
}

void ImGuiGui::render() {
    if (!m_valid || !m_frameActive)
        return;
    ImGui::Render(); // populates GetDrawData(); the renderer records it in its swapchain pass
    m_frameActive = false;
}

bool ImGuiGui::beginWindow(std::string_view title, float xN, float yN, float wN, float hN) {
    if (!m_valid)
        return false;
    ImGui::SetNextWindowPos(toPixels(xN, yN), ImGuiCond_Always);
    if (wN > 0.f && hN > 0.f)
        ImGui::SetNextWindowSize(toPixels(wN, hN), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    return ImGui::Begin(z(title), nullptr, flags);
}

void ImGuiGui::endWindow() {
    if (m_valid)
        ImGui::End();
}

void ImGuiGui::label(std::string_view text) {
    if (m_valid)
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void ImGuiGui::separator() {
    if (m_valid)
        ImGui::Separator();
}

void ImGuiGui::sameLine() {
    if (m_valid)
        ImGui::SameLine();
}

bool ImGuiGui::inputText(std::string_view label, char* buf, std::size_t cap, bool masked) {
    if (!m_valid || buf == nullptr || cap == 0)
        return false;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_None;
    if (masked)
        flags |= ImGuiInputTextFlags_Password;
    return ImGui::InputText(z(label), buf, cap, flags);
}

bool ImGuiGui::button(std::string_view label) {
    return m_valid && ImGui::Button(z(label));
}

bool ImGuiGui::selectable(std::string_view label, bool selected) {
    return m_valid && ImGui::Selectable(z(label), selected);
}

bool ImGuiGui::checkbox(std::string_view label, bool* value) {
    if (!m_valid || !value)
        return false;
    return ImGui::Checkbox(z(label), value);
}

bool ImGuiGui::treeNode(std::string_view id, std::string_view label, bool* selected, bool leaf) {
    if (!m_valid)
        return false;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected && *selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    // Push the id so duplicate node labels don't collide; render `label` as the visible text.
    ImGui::PushID(z(id));
    const bool open = ImGui::TreeNodeEx("", flags, "%s", z(label));
    if (selected && ImGui::IsItemClicked())
        *selected = true;
    ImGui::PopID();
    // A leaf uses NoTreePushOnOpen, so it never pushes the tree stack -> return false so the caller
    // does not call treePop() for it. A non-leaf returns whether it is open (caller pairs treePop()).
    return leaf ? false : open;
}

void ImGuiGui::treePop() {
    if (m_valid)
        ImGui::TreePop();
}

bool ImGuiGui::beginTable(std::string_view id, int columns) {
    if (!m_valid || columns < 1)
        return false;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    return ImGui::BeginTable(z(id), columns, flags);
}

void ImGuiGui::tableHeadersRow(std::span<const std::string_view> headers) {
    if (!m_valid)
        return;
    for (const std::string_view& h : headers)
        ImGui::TableSetupColumn(z(h));
    ImGui::TableHeadersRow();
}

void ImGuiGui::tableNextRow() {
    if (m_valid)
        ImGui::TableNextRow();
}

void ImGuiGui::tableCell(std::string_view text) {
    if (!m_valid)
        return;
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void ImGuiGui::endTable() {
    if (m_valid)
        ImGui::EndTable();
}

bool ImGuiGui::wantCaptureKeyboard() const {
    return m_valid && ImGui::GetIO().WantCaptureKeyboard;
}

bool ImGuiGui::wantCaptureMouse() const {
    return m_valid && ImGui::GetIO().WantCaptureMouse;
}

std::unique_ptr<IGui> createImGuiGui(IWindow& window, IRenderer& renderer) {
    auto gui = std::make_unique<ImGuiGui>(window, renderer);
    if (!gui->isValid())
        return nullptr;
    return gui;
}

} // namespace fl
