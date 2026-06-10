// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IInput.h"
#include "RenderTypes.h"
#include "debug/DebugCommandRegistry.h"

#include <array>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <string_view>

class ILogger;

// Quake-style drop-down debug console.
//
// Toggle:  backtick (`) — handled in main.cpp via SDL_SCANCODE_GRAVE.
// Close:   Escape — detected by tick(); caller should call close().
// Typing:  ITextInputHandler delivers printable chars; special keys via IInput.
//
// Rendering: buildHud() populates a pre-allocated HudElement array each frame.
// The position widget (top-right world coords) is emitted by buildHud() even
// when the console is closed, if m_showPos is true and a player pos is supplied.
class DebugConsole : public ITextInputHandler {
  public:
    explicit DebugConsole(ILogger& logger, DebugCommandRegistry& registry);

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
    // camPos:    camera world position — always shown at top-right when non-null.
    // playerPos: player entity world position — shown one line below camPos when
    //            showPos is true and non-null.
    void buildHud(const glm::dvec3* camPos = nullptr, const glm::dvec3* playerPos = nullptr);

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
    void execute(std::string_view line);

    // Append a line to the output ring without command dispatch (e.g. server notices).
    void print(std::string line);

    // Access the showPos flag so DebugCommands can toggle it.
    [[nodiscard]] bool& showPosRef() noexcept {
        return m_showPos;
    }

  private:
    static constexpr int kMaxOutputLines = 64;
    static constexpr int kVisibleLines = 20;
    static constexpr int kHistoryCap = 32;
    static constexpr int kMaxHudElems = 29; // rect + 2 lines + title + 20 output + prompt + pos
    static constexpr int kMaxStrings = 26;  // title + 20 output + prompt + pos + slack

    ILogger& m_logger;
    DebugCommandRegistry& m_registry;

    bool m_open{false};
    std::string m_input;

    // Output ring (circular, newest at head-1)
    std::array<std::string, kMaxOutputLines> m_outputRing;
    int m_outputHead{0};
    int m_outputCount{0};

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

    void pushOutput(std::string line);
    void submitLine();

    // HUD helpers — each returns false if the pre-allocated arrays are full
    bool pushText(float x, float y, float r, float g, float b, const char* fmt, ...);
    bool pushLine(float x0, float y0, float x1, float y1, float r, float g, float b);
    bool pushRect(float x0, float y0, float x1, float y1, float r, float g, float b, float a);
};
