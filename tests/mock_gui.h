// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IGui.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// Scripted NullGui (#156): a headless IGui that RECORDS the widget vocabulary a screen emits and returns
// PROGRAMMED results, so the join-server / server-browser / scoreboard surfaces are unit-tested with no
// window and no GPU — the mock_network.h / mock_renderer.h idiom applied to the GUI HAL.
//
// Usage in a test: set the scripted maps (buttonClicks["Connect"] = true; inputText["Address"] = "1.2.3.4"),
// call the screen's update() once (which drives one newFrame()/widgets/render()), then assert on the
// recorded vectors and the screen's resulting state. Call clear() to reset the per-frame records between
// simulated frames; the scripted maps persist until you change them.
class NullGui : public IGui {
  public:
    // ── Recorded emissions (assert on these) ─────────────────────────────────────────────────────
    std::vector<std::string> windows;     // beginWindow titles, in order
    std::vector<std::string> labels;      // label texts
    std::vector<std::string> buttons;     // button labels queried
    std::vector<std::string> selectables; // selectable labels queried
    std::vector<std::string> headers;     // table header cell texts (flattened)
    std::vector<std::string> cells;       // table body cell texts (flattened, row-major)
    int newFrameCount{0};
    int renderCount{0};
    int rowCount{0}; // tableNextRow() calls

    // ── Scripted responses ───────────────────────────────────────────────────────────────────────
    // button(label) / selectable(label) return true when the map holds true for that label.
    std::unordered_map<std::string, bool> buttonClicks;
    std::unordered_map<std::string, bool> selectableClicks;
    // inputText(label): if a value is queued for `label`, it is copied into the caller's buffer ONCE
    // (then consumed) and the call returns true (changed). Otherwise the buffer is left untouched.
    std::unordered_map<std::string, std::string> inputTextValues;
    bool captureKeyboard{false};
    bool captureMouse{false};

    // Reset the per-frame recorded emissions (keep the scripted maps). Call between simulated frames.
    void clear() {
        windows.clear();
        labels.clear();
        buttons.clear();
        selectables.clear();
        headers.clear();
        cells.clear();
        rowCount = 0;
    }

    void newFrame() override {
        ++newFrameCount;
    }
    void render() override {
        ++renderCount;
    }
    void processEvent(const void*) override {}

    bool beginWindow(std::string_view title, float, float, float, float) override {
        windows.emplace_back(title);
        return true;
    }
    void endWindow() override {}

    void label(std::string_view text) override {
        labels.emplace_back(text);
    }
    void separator() override {}
    void sameLine() override {}

    bool inputText(std::string_view lbl, char* buf, std::size_t cap, bool = false) override {
        const auto it = inputTextValues.find(std::string(lbl));
        if (it == inputTextValues.end() || cap == 0)
            return false;
        const std::size_t n = std::min(it->second.size(), cap - 1);
        std::memcpy(buf, it->second.data(), n);
        buf[n] = '\0';
        inputTextValues.erase(it); // one-shot: the edit happens once, like a user typing then stopping
        return true;
    }

    bool button(std::string_view lbl) override {
        buttons.emplace_back(lbl);
        const auto it = buttonClicks.find(std::string(lbl));
        return it != buttonClicks.end() && it->second;
    }

    bool selectable(std::string_view lbl, bool) override {
        selectables.emplace_back(lbl);
        const auto it = selectableClicks.find(std::string(lbl));
        return it != selectableClicks.end() && it->second;
    }

    bool beginTable(std::string_view, int) override {
        return true;
    }
    void tableHeadersRow(std::span<const std::string_view> hs) override {
        for (const auto& h : hs)
            headers.emplace_back(h);
    }
    void tableNextRow() override {
        ++rowCount;
    }
    void tableCell(std::string_view text) override {
        cells.emplace_back(text);
    }
    void endTable() override {}

    [[nodiscard]] bool wantCaptureKeyboard() const override {
        return captureKeyboard;
    }
    [[nodiscard]] bool wantCaptureMouse() const override {
        return captureMouse;
    }
};

} // namespace fl
