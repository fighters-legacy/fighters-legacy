// SPDX-License-Identifier: GPL-3.0-or-later
#include "job/CpuBudget.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

namespace fl {
namespace {

// Parse a leading non-negative integer. Returns false on anything else (including "max"), so the
// callers below never have to distinguish "unparseable" from "unlimited" -- both mean 0 CPUs.
bool parseLong(std::string_view s, long long& out) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    const char* first = s.data();
    const auto res = std::from_chars(first, first + s.size(), out);
    return res.ec == std::errc{} && res.ptr != first;
}

// quota/period as whole CPUs, rounded UP: a 1.5-CPU limit is 2 usable CPUs, and rounding down
// would size a 0.5-CPU pod's pool to zero.
unsigned cpusFromQuota(long long quotaUs, long long periodUs) noexcept {
    if (quotaUs <= 0 || periodUs <= 0)
        return 0u; // unlimited, or a nonsense pair we must not act on
    const long long cpus = (quotaUs + periodUs - 1) / periodUs;
    return static_cast<unsigned>(std::max<long long>(1, cpus));
}

std::string readFirstLine(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    std::string line;
    std::getline(in, line);
    return line;
}

const char* limitName(CpuBudgetLimit l) noexcept {
    switch (l) {
    case CpuBudgetLimit::Online:
        return "online cpus";
    case CpuBudgetLimit::Affinity:
        return "affinity";
    case CpuBudgetLimit::Quota:
        return "cgroup quota";
    case CpuBudgetLimit::Unknown:
        break;
    }
    return "nothing detectable";
}

} // namespace

unsigned parseCgroupV2CpuMax(std::string_view text) noexcept {
    // "$MAX $PERIOD", where $MAX is a number of microseconds or the literal "max" (unlimited).
    long long quota = 0;
    if (!parseLong(text, quota))
        return 0u; // "max", or a file we cannot read as a quota
    const auto sp = text.find(' ');
    long long period = 100000; // the kernel default, and what a truncated line means
    if (sp != std::string_view::npos)
        (void)parseLong(text.substr(sp + 1), period);
    return cpusFromQuota(quota, period);
}

unsigned parseCgroupV1CpuQuota(std::string_view quotaUs, std::string_view periodUs) noexcept {
    long long quota = 0;
    long long period = 0;
    if (!parseLong(quotaUs, quota) || !parseLong(periodUs, period))
        return 0u;
    return cpusFromQuota(quota, period); // v1 spells "unlimited" as quota = -1, which parses to <= 0
}

unsigned resolveCpuBudget(const CpuBudget& b) noexcept {
    unsigned n = 0;
    for (const unsigned c : {b.online, b.affinity, b.quota})
        if (c != 0u && (n == 0u || c < n))
            n = c;
    return n;
}

CpuBudgetLimit cpuBudgetLimit(const CpuBudget& b) noexcept {
    const unsigned n = resolveCpuBudget(b);
    if (n == 0u)
        return CpuBudgetLimit::Unknown;
    // A constraint only BINDS when it is strictly tighter than the machine's online count -- an
    // affinity mask covering every CPU is the default state, not a restriction, and reporting it as
    // one would tell an unconstrained operator their server had been limited. Among constraints
    // that do bind, a tie names the tighter-scoped one: a pod whose quota equals its mask was
    // limited by the quota someone set.
    const bool binds = (b.online == 0u) || (n < b.online);
    if (binds && b.quota == n)
        return CpuBudgetLimit::Quota;
    if (binds && b.affinity == n)
        return CpuBudgetLimit::Affinity;
    return CpuBudgetLimit::Online;
}

std::string describeCpuBudget(const CpuBudget& b) {
    const auto field = [](const char* name, unsigned v, const char* whenZero) {
        return std::string(name) + " " + (v == 0u ? whenZero : std::to_string(v));
    };
    std::string s = field("online", b.online, "unknown");
    s += ", " + field("affinity", b.affinity, "unrestricted");
    s += ", " + field("cgroup", b.quota, "unlimited");
    const unsigned n = resolveCpuBudget(b);
    s += " -> " + (n == 0u ? std::string("unknown") : std::to_string(n)) + " usable, limited by ";
    s += limitName(cpuBudgetLimit(b));
    return s;
}

unsigned cgroupV2QuotaAt(const std::string& root, const std::string& rel) {
    unsigned tightest = 0;
    std::string path = rel;
    for (;;) {
        const unsigned q = parseCgroupV2CpuMax(readFirstLine((root + path + "/cpu.max").c_str()));
        if (q != 0u && (tightest == 0u || q < tightest))
            tightest = q;
        if (path.empty())
            break;
        const auto slash = path.rfind('/');
        path = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
    }
    return tightest;
}

std::string cgroupV2RelPath() {
    std::ifstream in("/proc/self/cgroup");
    std::string line;
    while (std::getline(in, line)) {
        // v2 is the single "0::<path>" line; v1 lines carry a controller list in the middle field.
        if (line.rfind("0::", 0) == 0) {
            std::string rel = line.substr(3);
            if (rel == "/")
                return {}; // the mount root itself; the walk below reads it either way
            return rel;
        }
    }
    return {};
}

CpuBudget detectCpuBudget() {
    CpuBudget b;
    b.online = std::thread::hardware_concurrency();
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0)
            b.affinity = static_cast<unsigned>(n);
    }
    // cgroup v2 first: a unified-hierarchy host has no v1 files at all. Reading a file that is not
    // there simply leaves the quota unset (= unlimited), which is the right answer for a host with
    // no quota on it.
    b.quota = cgroupV2QuotaAt("/sys/fs/cgroup", cgroupV2RelPath());
    if (b.quota == 0u) {
        // cgroup v1, at the layout a container sees. Not walked: v1 spreads controllers across
        // separate mounts whose paths are not derivable without parsing /proc/self/mountinfo, and
        // the case that matters (a container's own limit) is at this fixed path.
        b.quota = parseCgroupV1CpuQuota(readFirstLine("/sys/fs/cgroup/cpu/cpu.cfs_quota_us"),
                                        readFirstLine("/sys/fs/cgroup/cpu/cpu.cfs_period_us"));
    }
#endif
    return b;
}

} // namespace fl
