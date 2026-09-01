// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace fl {

// The CPU budget the OS actually granted this process (#1380).
//
// `std::thread::hardware_concurrency()` reports the machine's ONLINE CPUs. It sees neither the
// process CPU-affinity mask nor a cgroup CPU quota, so a server pinned with `taskset -c 0-7`, or a
// pod with a CPU `limit`, sizes its worker pool for the whole box: 23 background workers onto 8
// usable CPUs, an oversubscription the server had no idea about. Oversubscribing a fork-join pass
// is worse than undersubscribing it -- every barrier then waits on threads competing for the same
// cores, which inflates exactly the tail latency the scale gate measures.
//
// Each field is a CPU count, and 0 means "this constraint said nothing" (unsupported platform,
// unreadable file, or genuinely unlimited) -- never "zero CPUs".
struct CpuBudget {
    unsigned online = 0;   // hardware_concurrency(): the machine's online CPUs
    unsigned affinity = 0; // CPUs in the process affinity mask (Linux: sched_getaffinity)
    unsigned quota = 0;    // CPUs from the cgroup CPU quota, rounded up (Linux: cgroup v2/v1)
};

// Which constraint decided the answer -- what the startup line names, so an operator can see that
// the server understood its budget rather than guessing at a number.
enum class CpuBudgetLimit { Unknown, Online, Affinity, Quota };

// Usable CPUs: the MINIMUM of every constraint that reported one, 0 when none did. Pure.
[[nodiscard]] unsigned resolveCpuBudget(const CpuBudget& b) noexcept;

// The constraint that produced resolveCpuBudget()'s answer. A constraint counts only when it is
// strictly tighter than the online count -- a full-width affinity mask is the default, not a
// restriction. Ties among binding constraints resolve to the tighter-scoped one (quota, then
// affinity): when a pod's quota equals its affinity, the quota is what an operator set. Pure.
[[nodiscard]] CpuBudgetLimit cpuBudgetLimit(const CpuBudget& b) noexcept;

// One-line human-readable budget, e.g. "online 24, affinity 8, cgroup unlimited -> 8 usable,
// limited by affinity". Pure.
[[nodiscard]] std::string describeCpuBudget(const CpuBudget& b);

// Read the budget from the OS.
//
// Linux: `sched_getaffinity()` for the mask, `/sys/fs/cgroup/cpu.max` (v2) or
// `cpu.cfs_quota_us`/`cpu.cfs_period_us` (v1) for the quota. Everywhere else: online CPUs only --
// neither Windows nor macOS has the cgroup problem, and Windows process affinity would be
// `GetProcessAffinityMask` if it ever matters.
[[nodiscard]] CpuBudget detectCpuBudget();

// Parse a cgroup v2 `cpu.max` line into CPUs, rounded UP ("max 100000" -> 0 = unlimited,
// "200000 100000" -> 2, "150000 100000" -> 2). 0 for unlimited or unparseable. Pure, so the quota
// arithmetic is testable without a container.
[[nodiscard]] unsigned parseCgroupV2CpuMax(std::string_view text) noexcept;

// Same, for cgroup v1's two files. A negative or absent quota is unlimited -> 0. Pure.
[[nodiscard]] unsigned parseCgroupV1CpuQuota(std::string_view quotaUs, std::string_view periodUs) noexcept;

// Walk a cgroup v2 tree from `rel` up to `root`, returning the TIGHTEST `cpu.max` found (0 = none).
//
// The quota that binds a process is not necessarily written on its own cgroup: a container sees its
// limit at the root of its namespace, while a service in a systemd slice inherits one from an
// ancestor. Reading only one level finds the pod case and misses the slice case, so both are walked.
// Parameterised on the mount root so the walk is testable against a temp directory tree.
[[nodiscard]] unsigned cgroupV2QuotaAt(const std::string& root, const std::string& rel);

// This process's cgroup v2 path relative to the mount root, read from /proc/self/cgroup ("0::<path>").
// Empty when there is no v2 line (v1-only host) or the file is unreadable.
[[nodiscard]] std::string cgroupV2RelPath();

} // namespace fl
