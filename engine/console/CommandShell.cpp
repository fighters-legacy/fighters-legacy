// SPDX-License-Identifier: GPL-3.0-or-later
#include "console/CommandShell.h"

#include "ILogger.h"
#include "console/CommandRegistry.h"

#include <string_view>

CommandShell::CommandShell(ILogger& logger, CommandRegistry& registry) : m_logger(logger), m_registry(registry) {}

void CommandShell::print(std::string line) {
    std::lock_guard<std::mutex> lk(m_ringMutex);
    pushOutput(std::move(line));
}

std::string CommandShell::execute(std::string_view line) {
    if (line.empty())
        return {};

    std::string result = m_registry.dispatch(line);

    {
        std::lock_guard<std::mutex> lk(m_ringMutex);
        pushOutput("> " + std::string(line));

        if (!result.empty()) {
            std::string_view rv(result);
            while (!rv.empty()) {
                auto nl = rv.find('\n');
                std::string_view piece = (nl == std::string_view::npos) ? rv : rv.substr(0, nl);
                if (!piece.empty())
                    pushOutput(std::string(piece));
                if (nl == std::string_view::npos)
                    break;
                rv = rv.substr(nl + 1);
            }
        }
    }

    m_logger.log(LogLevel::Debug, __FILE__, __LINE__, ("game console: " + std::string(line)).c_str());
    return result;
}

std::vector<std::string> CommandShell::outputLines() const {
    std::lock_guard<std::mutex> lk(m_ringMutex);
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(m_outputCount));
    for (int i = 0; i < m_outputCount; ++i) {
        int idx = ((m_outputHead - m_outputCount + i) % kMaxOutputLines + kMaxOutputLines) % kMaxOutputLines;
        out.push_back(m_outputRing[idx]);
    }
    return out;
}

void CommandShell::pushOutput(std::string line) {
    m_outputRing[m_outputHead] = std::move(line);
    m_outputHead = (m_outputHead + 1) % kMaxOutputLines;
    if (m_outputCount < kMaxOutputLines)
        ++m_outputCount;
    ++m_totalWritten;
}

int CommandShell::mark() const {
    std::lock_guard<std::mutex> lk(m_ringMutex);
    return m_totalWritten;
}

std::vector<std::string> CommandShell::drainSince(int mark) const {
    std::lock_guard<std::mutex> lk(m_ringMutex);
    int newCount = m_totalWritten - mark;
    if (newCount <= 0)
        return {};
    if (newCount > kMaxOutputLines)
        newCount = kMaxOutputLines;
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(newCount));
    for (int i = 0; i < newCount; ++i) {
        int idx = ((m_outputHead - newCount + i) % kMaxOutputLines + kMaxOutputLines) % kMaxOutputLines;
        out.push_back(m_outputRing[idx]);
    }
    return out;
}
