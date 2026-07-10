// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/ProcessStats.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// <psapi.h> must follow <windows.h>.
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace fl {

uint64_t currentRssKb() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<uint64_t>(pmc.WorkingSetSize) / 1024u;
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<uint64_t>(info.resident_size) / 1024u;
    return 0;
#else
    // Linux: /proc/self/statm — the second field is the resident set size in pages.
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    unsigned long long sizePages = 0, residentPages = 0;
    const int n = std::fscanf(f, "%llu %llu", &sizePages, &residentPages);
    std::fclose(f);
    if (n < 2)
        return 0;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0)
        return 0;
    return residentPages * static_cast<uint64_t>(pageSize) / 1024u;
#endif
}

} // namespace fl
