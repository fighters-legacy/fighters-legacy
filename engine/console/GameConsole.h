// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IInput.h"
#include "RenderTypes.h"
#include "console/CommandShell.h"

#include <array>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <string_view>

namespace fl {

// Quake-style drop-down game console.
//
// Toggle:  backtick (`) -- handled in main.cpp via SDL_SCANCODE_GRAVE.
// Close:   Escape -- detected by tick(); caller should call close().
// Typing:  ITextInputHandler delivers printable chars; special keys via IInput.
//
// Rendering: buildHud() populates a pre-allocated HudElement array each frame.
// The entity position widget (top-right world coords) is emitted by buildHud()
// even when the console is closed, if m_showPos is true and a player pos is supplied.
class GameConsole : public CommandShell, public ITextInputHandler {
  public:
    explicit GameConsole(ILogger& logger, CommandRegistry& registry);

    // -----------------------------------------------------------------------
    // Lifecycle (main thread)
    // -----------------------------------------------------------------------

    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }

    // Enable text input delivery from the OS IME pipeline.
    void open(IInput& input);

    // Disable text input delivery.
    void close(IInput& input);

    // For tests: open without touching IInput (no SDL text-input started).
    void openHeadless() noexcept {
        m_open = true;
    }

    // -----------------------------------------------------------------------
    // Per-frame (main thread, between beginFrame / endFrame)
    // -----------------------------------------------------------------------

    // Process special keys (Enter, Backspace, Up/Down, Escape) via
    // IInput::isKeyJustPressed(). Returns true if Escape was pressed.
    bool tick(IInput& input);

    // Rebuild HudElement list for this frame.
    // playerPos: player entity world position -- shown top-right when showPos (toggle_pos)
    //            is true and non-null. Camera/entity debug readouts live in the F3 overlay.
    void buildHud(const glm::dvec3* playerPos = nullptr);

    [[nodiscard]] std::span<const HudElement> elements() const {
        return {m_elems.data(), static_cast<std::size_t>(m_elemCount)};
    }

    // -----------------------------------------------------------------------
    // ITextInputHandler
    // -----------------------------------------------------------------------
    void onTextInput(const char* text) override;
    void onTextEdit(const char* comp, int start) override;

    // -----------------------------------------------------------------------
    // Public test / command helpers
    // -----------------------------------------------------------------------

    // Dispatch a line directly (bypasses input buffer; records history + output).
    // Overrides CommandShell::execute() to also record command history.
    std::string execute(std::string_view line) override;

    // Access the showPos flag so ConsoleCommands can toggle it.
    [[nodiscard]] bool& showPosRef() noexcept {
        return m_showPos;
    }

  private:
    static constexpr int kVisibleLines = 20;
    static constexpr int kHistoryCap = 32;
    static constexpr int kMaxHudElems = 29; // rect + 2 lines + title + 20 output + prompt + pos
    static constexpr int kMaxStrings = 26;  // title + 20 output + prompt + pos + slack

    bool m_open{false};
    std::string m_input;

    // Command history
    std::array<std::string, kHistoryCap> m_history;
    int m_historyHead{0};
    int m_historyCount{0};
    int m_historyIdx{-1}; // -1 = editing current input; >= 0 = browsing history

    bool m_showPos{false};

    // HUD storage (string_views reference these; valid until next buildHud())
    std::array<HudElement, kMaxHudElems> m_elems;
    std::array<std::string, kMaxStrings> m_strings;
    int m_elemCount{0};
    int m_strCount{0};

    void submitLine();

    // HUD helpers -- each returns false if the pre-allocated arrays are full
    bool pushText(float x, float y, float r, float g, float b, const char* fmt, ...);
    bool pushLine(float x0, float y0, float x1, float y1, float r, float g, float b);
    bool pushRect(float x0, float y0, float x1, float y1, float r, float g, float b, float a);
};

} // namespace fl
