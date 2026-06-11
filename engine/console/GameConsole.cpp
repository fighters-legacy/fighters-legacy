// SPDX-License-Identifier: GPL-3.0-or-later
#include "console/GameConsole.h"

#include "ILogger.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

// HUD color constants
static constexpr float kGreenR = 0.0f, kGreenG = 1.0f, kGreenB = 0.0f;
static constexpr float kDimR = 0.7f, kDimG = 0.7f, kDimB = 0.7f;
static constexpr float kBrightR = 1.0f, kBrightG = 1.0f, kBrightB = 1.0f;

// Half-screen layout constants
static constexpr float kPanelTop = 0.0f;
static constexpr float kPanelBot = 0.50f;
static constexpr float kSepTop = 0.028f;
static constexpr float kSepBot = 0.450f;
static constexpr float kTitleY = 0.008f;
static constexpr float kFirstLineY = 0.038f;
static constexpr float kLineStep = 0.020f;
static constexpr float kPromptY = 0.464f;
static constexpr float kPosX = 0.62f;
static constexpr float kPosY = 0.008f;
static constexpr float kTextX = 0.01f;

GameConsole::GameConsole(ILogger& logger, CommandRegistry& registry) : CommandShell(logger, registry) {}

void GameConsole::open(IInput& input) {
    m_open = true;
    input.startTextInput(this);
}

void GameConsole::close(IInput& input) {
    m_open = false;
    m_historyIdx = -1;
    input.stopTextInput();
}

bool GameConsole::tick(IInput& input) {
    if (input.isKeyJustPressed(Key::Escape))
        return true;

    if (input.isKeyJustPressed(Key::Enter)) {
        submitLine();
        return false;
    }

    if (input.isKeyJustPressed(Key::Backspace)) {
        if (!m_input.empty())
            m_input.pop_back();
        return false;
    }

    // History navigation
    if (input.isKeyJustPressed(Key::ArrowUp)) {
        int available = std::min(m_historyCount, kHistoryCap);
        if (available > 0 && m_historyIdx < available - 1) {
            ++m_historyIdx;
            int slot = ((m_historyHead - 1 - m_historyIdx) % kHistoryCap + kHistoryCap) % kHistoryCap;
            m_input = m_history[slot];
        }
    }

    if (input.isKeyJustPressed(Key::ArrowDown)) {
        if (m_historyIdx > 0) {
            --m_historyIdx;
            int slot = ((m_historyHead - 1 - m_historyIdx) % kHistoryCap + kHistoryCap) % kHistoryCap;
            m_input = m_history[slot];
        } else if (m_historyIdx == 0) {
            m_historyIdx = -1;
            m_input.clear();
        }
    }

    return false;
}

void GameConsole::onTextInput(const char* text) {
    if (!m_open || !text)
        return;
    // Command input is ASCII identifiers; non-ASCII bytes from IME/paste are dropped.
    for (const char* p = text; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 0x20 && c <= 0x7E)
            m_input += static_cast<char>(c);
    }
    m_historyIdx = -1;
}

void GameConsole::onTextEdit(const char* /*comp*/, int /*start*/) {
    // IME composition preview -- not shown in current implementation
}

std::string GameConsole::execute(std::string_view line) {
    if (line.empty())
        return {};

    std::string result = CommandShell::execute(line);

    // Record history (skip duplicates of the most-recent entry)
    std::string lineStr(line);
    bool isDupe = false;
    if (m_historyCount > 0) {
        int prev = ((m_historyHead - 1) % kHistoryCap + kHistoryCap) % kHistoryCap;
        isDupe = (m_history[prev] == lineStr);
    }
    if (!isDupe) {
        m_history[m_historyHead % kHistoryCap] = std::move(lineStr);
        m_historyHead = (m_historyHead + 1) % kHistoryCap;
        if (m_historyCount < kHistoryCap)
            ++m_historyCount;
    }
    m_historyIdx = -1;

    return result;
}

void GameConsole::submitLine() {
    std::string line = m_input;
    m_input.clear();
    m_historyIdx = -1;
    if (!line.empty())
        execute(line);
}

// ---------------------------------------------------------------------------
// HUD helpers
// ---------------------------------------------------------------------------

bool GameConsole::pushText(float x, float y, float r, float g, float b, const char* fmt, ...) {
    if (m_elemCount >= kMaxHudElems || m_strCount >= kMaxStrings)
        return false;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_strings[m_strCount] = buf;
    HudElement& el = m_elems[m_elemCount++];
    el.type = HudElement::Type::Text;
    el.x = x;
    el.y = y;
    el.r = r;
    el.g = g;
    el.b = b;
    el.a = 1.0f;
    el.scale = 1.0f;
    el.text = m_strings[m_strCount++];
    return true;
}

bool GameConsole::pushLine(float x0, float y0, float x1, float y1, float r, float g, float b) {
    if (m_elemCount >= kMaxHudElems)
        return false;
    HudElement& el = m_elems[m_elemCount++];
    el.type = HudElement::Type::Line;
    el.x = x0;
    el.y = y0;
    el.x2 = x1;
    el.y2 = y1;
    el.strokeWidth = 1.0f;
    el.r = r;
    el.g = g;
    el.b = b;
    el.a = 1.0f;
    return true;
}

bool GameConsole::pushRect(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    if (m_elemCount >= kMaxHudElems)
        return false;
    HudElement& el = m_elems[m_elemCount++];
    el.type = HudElement::Type::Rect;
    el.x = x0;
    el.y = y0;
    el.x2 = x1;
    el.y2 = y1;
    el.r = r;
    el.g = g;
    el.b = b;
    el.a = a;
    return true;
}

// ---------------------------------------------------------------------------
// buildHud
// ---------------------------------------------------------------------------

void GameConsole::buildHud(const glm::dvec3* camPos, const glm::dvec3* playerPos) {
    m_elemCount = 0;
    m_strCount = 0;

    // Camera position -- always visible when supplied; 2x scale for readability
    if (camPos) {
        pushText(kPosX, kPosY, 1.0f, 0.2f, 0.2f, "CAM %+.0f %+.0f %+.0f", static_cast<float>(camPos->x),
                 static_cast<float>(camPos->y), static_cast<float>(camPos->z));
        m_elems[m_elemCount - 1].scale = 2.0f;
    }

    // Entity position -- 3x line-step below to clear the 2x cam text
    if (m_showPos && playerPos) {
        pushText(kPosX, kPosY + kLineStep * 3.0f, 0.0f, 0.7f, 0.0f, "ENT %+.0f %+.0f %+.0f",
                 static_cast<float>(playerPos->x), static_cast<float>(playerPos->y), static_cast<float>(playerPos->z));
    }

    if (!m_open)
        return;

    // Background panel
    pushRect(0.0f, kPanelTop, 1.0f, kPanelBot, 0.05f, 0.05f, 0.05f, 0.88f);

    // Title
    pushText(kTextX, kTitleY, kGreenR, kGreenG, kGreenB, "%s", "FIGHTERS LEGACY -- GAME CONSOLE");

    // Top separator
    pushLine(0.0f, kSepTop, 1.0f, kSepTop, kGreenR, kGreenG, kGreenB);

    // Output lines -- show the last kVisibleLines entries, oldest at top
    {
        std::lock_guard<std::mutex> lk(m_ringMutex);
        int show = std::min(m_outputCount, kVisibleLines);
        // Index of oldest visible entry
        int oldest = ((m_outputHead - show) % kMaxOutputLines + kMaxOutputLines) % kMaxOutputLines;
        for (int i = 0; i < show; ++i) {
            int idx = (oldest + i) % kMaxOutputLines;
            float y = kFirstLineY + static_cast<float>(i) * kLineStep;
            // Newest two lines are full-bright, older are dimmed
            bool bright = (i >= show - 2);
            float cr = bright ? kBrightR : kDimR;
            float cg = bright ? kBrightG : kDimG;
            float cb = bright ? kBrightB : kDimB;
            pushText(kTextX, y, cr, cg, cb, "%s", m_outputRing[idx].c_str());
        }
    }

    // Bottom separator
    pushLine(0.0f, kSepBot, 1.0f, kSepBot, kGreenR, kGreenG, kGreenB);

    // Input prompt with underscore cursor
    pushText(kTextX, kPromptY, kGreenR, kGreenG, kGreenB, "> %s_", m_input.c_str());
}
