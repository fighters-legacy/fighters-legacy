// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReplaySelectScreen.h"
#include "MenuNav.h"
#include "render/HudBuilder.h" // hudFullscreenBg (#1261)

#include "IInput.h"
#include "IWindow.h"
#include "replay/ReplayReader.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <system_error>

namespace fl {

namespace {

std::string formatDuration(double seconds) {
    if (seconds < 0.0)
        seconds = 0.0;
    const auto total = static_cast<int>(seconds);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%dm%02ds", total / 60, total % 60);
    return buf;
}

std::string formatStamp(uint64_t unixSeconds) {
    if (unixSeconds == 0)
        return "unknown date";
    const auto t = static_cast<std::time_t>(unixSeconds);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d %b %H:%M", &tmv);
    return buf;
}

} // namespace

std::vector<ReplaySelectScreen::Entry> ReplaySelectScreen::scan(const std::filesystem::path& dir) {
    std::vector<Entry> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return out; // no recordings yet is not an error

    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!e.is_regular_file(ec) || e.path().extension() != ".flrep")
            continue;
        const auto when = std::filesystem::last_write_time(e.path(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        files.emplace_back(when, e.path());
    }
    // Newest first: the replay a player wants is almost always the one they just flew.
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second > b.second;
    });

    out.reserve(files.size());
    for (const auto& [when, path] : files) {
        (void)when;
        Entry entry;
        entry.path = path;

        ReplayReader r;
        if (r.open(path)) {
            entry.playable = true;
            const std::string mission = r.header().missionId.empty() ? "free flight" : r.header().missionId;
            entry.label = formatStamp(r.header().startUnixSeconds) + "   " + mission + "   " +
                          formatDuration(r.durationSeconds());
            entry.detail = "recorded by " + (r.header().engineVersion.empty() ? std::string("an unknown build")
                                                                              : r.header().engineVersion);
            if (r.indexRebuilt())
                entry.detail += " (interrupted recording)"; // playable, but say so
        } else {
            entry.playable = false;
            entry.label = path.filename().string();
            entry.detail = r.lastError();
        }
        out.push_back(std::move(entry));
    }
    return out;
}

ReplaySelectScreen::ReplaySelectScreen(std::vector<Entry> entries) : m_entries(std::move(entries)) {}

Screen ReplaySelectScreen::update(IInput& input, IWindow& window) {
    const int n = static_cast<int>(m_entries.size());

    if (input.isKeyJustPressed(Key::Escape))
        return Screen::MainMenu;

    if (n == 0)
        return Screen::ReplaySelect;

    menuNavigateScrolled(input, n, kVisible, m_selectedIdx, m_scrollOffset);
    menuHoverHitTest(
        input, window, kVisible, m_scrollOffset, n, 0.055f,
        [](int r) { return 0.25f + static_cast<float>(r) * 0.065f; }, m_selectedIdx);

    if (menuConfirmPressed(input) && m_selectedIdx >= 0 && m_selectedIdx < n) {
        const Entry& e = m_entries[static_cast<std::size_t>(m_selectedIdx)];
        if (!e.playable) {
            // Refuse loudly and stay put, rather than entering a session that cannot start.
            m_status = "Cannot play this replay: " + e.detail;
            return Screen::ReplaySelect;
        }
        m_selected = e.path;
        m_status.clear();
        return Screen::Loading;
    }

    return Screen::ReplaySelect;
}

std::span<const HudElement> ReplaySelectScreen::buildElements() {
    m_elementCount = 0;

    // ⚠ WHITE, not black like the other seven screens' backgrounds. This block never set r/g/b, so
    // they kept HudElement's 1.f defaults -- almost certainly unintentional, but it is what ships
    // today, so #1261 preserves it exactly rather than changing what the screen looks like inside a
    // consolidation PR. Flagged for a decision.
    m_elements[static_cast<std::size_t>(m_elementCount++)] = hudFullscreenBg(1.f, 1.f, 1.f, 1.f);

    m_strings[0] = "REPLAYS";
    auto& title = m_elements[static_cast<std::size_t>(m_elementCount++)];
    title = HudElement{};
    title.type = HudElement::Type::Text;
    title.text = m_strings[0];
    title.x = 0.5f;
    title.align = HudAlign::Center;
    title.y = 0.1f;
    title.scale = 1.5f;
    title.r = title.g = title.b = title.a = 1.f;

    if (m_entries.empty()) {
        m_strings[1] = "No recordings yet.";
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[1];
        el.x = 0.5f;
        el.align = HudAlign::Center;
        el.y = 0.5f;
        el.r = el.g = el.b = 0.6f;
        el.a = 1.f;
        return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
    }

    const int n = static_cast<int>(m_entries.size());
    int si = 2;
    for (int i = 0; i < kVisible && m_elementCount + 1 < kMaxElements; ++i) {
        const int idx = m_scrollOffset + i;
        if (idx >= n)
            break;
        const Entry& e = m_entries[static_cast<std::size_t>(idx)];
        const bool sel = (idx == m_selectedIdx);
        const float y = 0.25f + static_cast<float>(i) * 0.065f;

        m_strings[static_cast<std::size_t>(si)] = e.label;
        auto& row = m_elements[static_cast<std::size_t>(m_elementCount++)];
        row = HudElement{};
        row.type = HudElement::Type::Text;
        row.text = m_strings[static_cast<std::size_t>(si++)];
        row.x = 0.5f;
        row.align = HudAlign::Center;
        row.y = y;
        row.a = 1.f;
        if (!e.playable) {
            row.r = 0.6f; // greyed, but listed: a file that cannot be read must still be visible
            row.g = 0.35f;
            row.b = 0.35f;
        } else if (sel) {
            row.r = 0.2f;
            row.g = 1.f;
            row.b = 0.2f;
        } else {
            row.r = row.g = row.b = 0.8f;
        }

        if (sel && m_elementCount < kMaxElements) {
            m_strings[static_cast<std::size_t>(si)] = e.detail;
            auto& det = m_elements[static_cast<std::size_t>(m_elementCount++)];
            det = HudElement{};
            det.type = HudElement::Type::Text;
            det.text = m_strings[static_cast<std::size_t>(si++)];
            det.x = 0.5f;
            det.align = HudAlign::Center;
            det.y = y + 0.028f;
            det.scale = 0.75f;
            det.r = det.g = det.b = 0.55f;
            det.a = 1.f;
        }
    }

    if (!m_status.empty() && m_elementCount < kMaxElements) {
        m_strings[static_cast<std::size_t>(si)] = m_status;
        auto& st = m_elements[static_cast<std::size_t>(m_elementCount++)];
        st = HudElement{};
        st.type = HudElement::Type::Text;
        st.text = m_strings[static_cast<std::size_t>(si)];
        st.x = 0.5f;
        st.align = HudAlign::Center;
        st.y = 0.92f;
        st.r = 1.f;
        st.g = 0.4f;
        st.b = 0.4f;
        st.a = 1.f;
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}

} // namespace fl
