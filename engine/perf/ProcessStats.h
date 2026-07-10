// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Current process resident set size (RSS) in KiB, or 0 when unavailable on this platform.
// Backends: Linux /proc/self/statm, macOS task_info, Windows GetProcessMemoryInfo. All platform
// specifics are confined to ProcessStats.cpp (the platform/Subprocess convention). Used by
// fl-server to self-report RSS in the tick metrics for the soak leak gate (#707).
[[nodiscard]] uint64_t currentRssKb() noexcept;

} // namespace fl
