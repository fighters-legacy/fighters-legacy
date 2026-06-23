// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

struct CrashInfo {
    const char* engineVersion{};
    char osInfo[256]{};
    char gpuInfo[256]{};

    struct ModEntry {
        char id[64];
        char version[32];
    };
    static constexpr int kMaxMods = 64;
    ModEntry mods[kMaxMods];
    int modCount{0};

    // Populates osInfo from the host OS (uname/sysctl/GetVersionEx).
    void populateOS();
};

} // namespace fl
