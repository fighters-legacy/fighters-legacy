// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace fl {

// A narrow immediate-mode GUI HAL — the IRenderer pattern applied to UI (#156, from epic #593).
//
// Input-heavy multiplayer surfaces (the join-server screen, the in-game server browser, chat input,
// the scoreboard) need real text entry, IME, tables and hit-testing that the hand-rolled HudElement
// path does not provide. Rather than couple those screens to Dear ImGui directly, they speak this
// backend-agnostic vocabulary: the reference backend (platform-gui, `ImGuiGui`) implements it over Dear
// ImGui + the SDL3/Vulkan backends, and a scripted `NullGui` drives the screens in unit tests with no
// window or GPU. It is DELIBERATELY NOT a full ImGui wrapper — the vocabulary grows only as consumers
// need it, and Phase 6 migrates the remaining HudElement screens onto it.
//
// Coordinates are normalized screen fractions in [0,1] (origin top-left), so a screen lays out the same
// at any resolution. All widget ids/labels are UTF-8. Threading: main thread only, between the backend's
// event pump and the renderer's frame submission.
class IGui {
  public:
    virtual ~IGui() = default;

    // ── Frame lifecycle ────────────────────────────────────────────────────────────────────────
    // newFrame() opens a UI frame (call AFTER the backend has forwarded this frame's window events);
    // widget calls accumulate between it and render(), which hands the accumulated draw data to the
    // renderer for compositing into the swapchain. Exactly one newFrame()/render() pair per rendered
    // frame, even when nothing is drawn (so the backend stays in a consistent state).
    virtual void newFrame() = 0;
    virtual void render() = 0;

    // Feed one platform window event to the GUI (an opaque `SDL_Event*` for the reference backend), so
    // it can update its keyboard/mouse/IME state and set the capture flags. Called from the window's
    // event pump before the game's own input sinks. Opaque by design — the HAL exposes no SDL types.
    virtual void processEvent(const void* nativeEvent) = 0;

    // ── Layout ─────────────────────────────────────────────────────────────────────────────────
    // A top-level window at normalized (xN,yN) with normalized size (wN,hN); a zero size auto-fits the
    // content. Returns true when the body is visible (emit content only then). ALWAYS pair with
    // endWindow(), even when beginWindow() returned false (mirrors ImGui's Begin/End contract).
    virtual bool beginWindow(std::string_view title, float xN, float yN, float wN, float hN) = 0;
    virtual void endWindow() = 0;

    // Static text / a horizontal rule / same-line layout of the next widget (e.g. Connect | Cancel).
    virtual void label(std::string_view text) = 0;
    virtual void separator() = 0;
    virtual void sameLine() = 0;

    // ── Interactive widgets ────────────────────────────────────────────────────────────────────
    // A single-line editable field over the caller-owned NUL-terminated buffer `buf` (capacity `cap`
    // bytes incl. the terminator). `masked` renders the contents as dots (passwords). Returns true on
    // any frame the contents changed. The backend clamps to `cap`.
    virtual bool inputText(std::string_view label, char* buf, std::size_t cap, bool masked = false) = 0;

    // A push button; returns true only on the frame it is activated.
    virtual bool button(std::string_view label) = 0;

    // A full-width selectable row (the server-browser / list idiom): `selected` is its current visual
    // state; returns true only on the frame it is clicked. Columns are aligned by the caller within the
    // label (this HAL stays list-oriented; the table below is read-only display).
    virtual bool selectable(std::string_view label, bool selected) = 0;

    // A labelled on/off checkbox bound to *value; returns true only on the frame it toggles (#838).
    virtual bool checkbox(std::string_view label, bool* value) = 0;

    // ── Hierarchy tree (fl-viewer node panel, #838) ─────────────────────────────────────────────
    // One row of a collapsible tree at the current depth. `id` disambiguates duplicate labels (glTF
    // nodes may share names). Returns true when the row is OPEN — the caller then emits its children
    // and calls treePop(). A `leaf` row draws without an arrow, never opens, and its treeNode() returns
    // false (so the caller does NOT call treePop() for it). `selected` (may be null) reflects and
    // receives click-selection: it is set true on the frame the row is clicked.
    virtual bool treeNode(std::string_view id, std::string_view label, bool* selected, bool leaf) = 0;
    virtual void treePop() = 0;

    // ── Read-only table (scoreboard / stats display) ───────────────────────────────────────────
    // beginTable() returns true when visible; then tableHeadersRow(headers) once, then per row
    // tableNextRow() followed by exactly `columns` tableCell() calls. Always endTable() when beginTable
    // returned true.
    virtual bool beginTable(std::string_view id, int columns) = 0;
    virtual void tableHeadersRow(std::span<const std::string_view> headers) = 0;
    virtual void tableNextRow() = 0;
    virtual void tableCell(std::string_view text) = 0;
    virtual void endTable() = 0;

    // ── Input capture ──────────────────────────────────────────────────────────────────────────
    // True when the GUI owns keyboard / mouse this frame — the game must then suppress the matching
    // flight/camera input (e.g. not fire the gun on Space while a chat box is focused).
    [[nodiscard]] virtual bool wantCaptureKeyboard() const = 0;
    [[nodiscard]] virtual bool wantCaptureMouse() const = 0;
};

} // namespace fl
