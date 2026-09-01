// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "job/CpuBudget.h"
#include "job/JobSystem.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using fl::CpuBudget;
using fl::CpuBudgetLimit;
using fl::JobSystem;
using fl::resolveWorkerCount;

TEST_CASE("resolveWorkerCount maps requested totals to background worker counts", "[job]") {
    // Explicit serial.
    CHECK(resolveWorkerCount(1, 8) == 0u);
    // N total -> N-1 background, independent of detected.
    CHECK(resolveWorkerCount(2, 8) == 1u);
    CHECK(resolveWorkerCount(8, 8) == 7u);
    CHECK(resolveWorkerCount(4, 1) == 3u);
    // Auto uses detected-1.
    CHECK(resolveWorkerCount(0, 8) == 7u);
    CHECK(resolveWorkerCount(0, 2) == 1u);
    // Auto on a single core degenerates to inline.
    CHECK(resolveWorkerCount(0, 1) == 0u);
    // Auto with undetectable hardware concurrency falls back to 4 total -> 3 background.
    CHECK(resolveWorkerCount(0, 0) == 3u);
}

TEST_CASE("JobSystem(1) runs inline with no background workers", "[job]") {
    JobSystem js(1);
    CHECK(js.workerCount() == 0u);

    std::vector<int> v(1000, 0);
    js.parallel_for(v.size(), 64, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            v[i] = static_cast<int>(i) + 1;
    });
    for (std::size_t i = 0; i < v.size(); ++i)
        REQUIRE(v[i] == static_cast<int>(i) + 1);
}

TEST_CASE("parallel_for visits every index exactly once across worker counts", "[job]") {
    for (unsigned total : {1u, 2u, 4u, 8u}) {
        JobSystem js(total);
        const std::size_t n = 10000;
        std::vector<std::atomic<int>> hits(n);
        for (auto& h : hits)
            h.store(0);

        js.parallel_for(n, 33, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                hits[i].fetch_add(1, std::memory_order_relaxed);
        });

        for (std::size_t i = 0; i < n; ++i)
            REQUIRE(hits[i].load() == 1);
    }
}

TEST_CASE("parallel_for produces the correct sum (parallel reduction via atomics)", "[job]") {
    JobSystem js(4);
    std::vector<std::size_t> data(5000);
    std::iota(data.begin(), data.end(), std::size_t{1});
    const std::size_t expected = std::accumulate(data.begin(), data.end(), std::size_t{0});

    std::atomic<std::size_t> sum{0};
    js.parallel_for(data.size(), 50, [&](std::size_t b, std::size_t e) {
        std::size_t local = 0;
        for (std::size_t i = b; i < e; ++i)
            local += data[i];
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    REQUIRE(sum.load() == expected);
}

TEST_CASE("parallel_for handles grain edge cases", "[job]") {
    JobSystem js(4);

    SECTION("count zero is a no-op and never invokes fn") {
        std::atomic<int> calls{0};
        js.parallel_for(0, 16, [&](std::size_t, std::size_t) { calls.fetch_add(1); });
        CHECK(calls.load() == 0);
    }

    SECTION("count smaller than grain runs as a single chunk") {
        std::vector<int> v(5, 0);
        js.parallel_for(v.size(), 64, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                v[i] = 7;
        });
        for (int x : v)
            CHECK(x == 7);
    }

    SECTION("grain zero is clamped to one and still covers the range") {
        std::vector<int> v(100, 0);
        js.parallel_for(v.size(), 0, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                v[i] = 1;
        });
        for (int x : v)
            CHECK(x == 1);
    }

    SECTION("non-divisible count covers the remainder chunk") {
        const std::size_t n = 1003; // not a multiple of grain
        std::vector<std::atomic<int>> hits(n);
        for (auto& h : hits)
            h.store(0);
        js.parallel_for(n, 100, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                hits[i].fetch_add(1);
        });
        for (std::size_t i = 0; i < n; ++i)
            CHECK(hits[i].load() == 1);
    }
}

TEST_CASE("parallel_for rethrows a chunk exception and the pool survives", "[job]") {
    JobSystem js(4);

    REQUIRE_THROWS_AS(js.parallel_for(1000, 50,
                                      [&](std::size_t b, std::size_t /*e*/) {
                                          if (b == 0)
                                              throw std::runtime_error("boom");
                                      }),
                      std::runtime_error);

    // The pool is reusable after an exception.
    std::vector<int> v(500, 0);
    js.parallel_for(v.size(), 32, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            v[i] = 3;
    });
    for (int x : v)
        REQUIRE(x == 3);
}

TEST_CASE("JobSystem survives many small back-to-back dispatches", "[job]") {
    // Stress the latch/condition-variable wakeup path; the meaningful target under TSan/ASan.
    JobSystem js(8);
    std::atomic<std::size_t> total{0};
    for (int iter = 0; iter < 2000; ++iter) {
        js.parallel_for(64, 8,
                        [&](std::size_t b, std::size_t e) { total.fetch_add(e - b, std::memory_order_relaxed); });
    }
    REQUIRE(total.load() == static_cast<std::size_t>(2000) * 64u);
}

// --- CPU budget (#1380) ---------------------------------------------------------------------
// hardware_concurrency() reports the machine's ONLINE CPUs and sees neither the process affinity
// mask nor a cgroup quota, so `taskset -c 0-7` got 23 background workers onto 8 usable CPUs. The
// pool must size to what the OS granted, and an explicit sim_worker_threads must still win.

TEST_CASE("resolveCpuBudget takes the tightest constraint that reported one", "[job][cpubudget]") {
    // Nothing constrained: the online count stands.
    CHECK(fl::resolveCpuBudget(CpuBudget{24, 0, 0}) == 24u);
    // taskset -c 0-7 on a 24-CPU box: the mask, not the box.
    CHECK(fl::resolveCpuBudget(CpuBudget{24, 8, 0}) == 8u);
    // A k8s pod with a CPU limit on a 24-core node: the quota, not the node.
    CHECK(fl::resolveCpuBudget(CpuBudget{24, 0, 4}) == 4u);
    // Both: whichever binds harder, in either direction.
    CHECK(fl::resolveCpuBudget(CpuBudget{24, 8, 4}) == 4u);
    CHECK(fl::resolveCpuBudget(CpuBudget{24, 2, 4}) == 2u);
    // A mask WIDER than the online count never invents CPUs.
    CHECK(fl::resolveCpuBudget(CpuBudget{4, 64, 0}) == 4u);
    // Nothing detectable at all stays 0 -- "unknown", which resolveWorkerCount turns into its
    // 4-thread fallback rather than a zero-sized pool.
    CHECK(fl::resolveCpuBudget(CpuBudget{}) == 0u);
    // An undetectable online count must not veto a constraint that DID report.
    CHECK(fl::resolveCpuBudget(CpuBudget{0, 8, 0}) == 8u);
}

TEST_CASE("cpuBudgetLimit names the constraint that decided the count", "[job][cpubudget]") {
    CHECK(fl::cpuBudgetLimit(CpuBudget{24, 0, 0}) == CpuBudgetLimit::Online);
    // A mask covering every CPU is the default state, not a restriction: an unconstrained host must
    // not be told it was "limited by affinity", which is what every Linux box reports.
    CHECK(fl::cpuBudgetLimit(CpuBudget{24, 24, 0}) == CpuBudgetLimit::Online);
    CHECK(fl::cpuBudgetLimit(CpuBudget{24, 8, 0}) == CpuBudgetLimit::Affinity);
    CHECK(fl::cpuBudgetLimit(CpuBudget{24, 0, 4}) == CpuBudgetLimit::Quota);
    // A tie names the tighter-scoped constraint: a pod whose quota equals its mask was limited by
    // the quota the operator set.
    CHECK(fl::cpuBudgetLimit(CpuBudget{24, 4, 4}) == CpuBudgetLimit::Quota);
    // With no online count to compare against, a constraint that reported still names itself.
    CHECK(fl::cpuBudgetLimit(CpuBudget{0, 8, 0}) == CpuBudgetLimit::Affinity);
    CHECK(fl::cpuBudgetLimit(CpuBudget{}) == CpuBudgetLimit::Unknown);
}

TEST_CASE("describeCpuBudget states the count and why", "[job][cpubudget]") {
    // The startup line an operator reads: every input, the answer, and the constraint.
    CHECK(fl::describeCpuBudget(CpuBudget{24, 24, 0}) ==
          "online 24, affinity 24, cgroup unlimited -> 24 usable, limited by online cpus");
    CHECK(fl::describeCpuBudget(CpuBudget{24, 8, 0}) ==
          "online 24, affinity 8, cgroup unlimited -> 8 usable, limited by affinity");
    CHECK(fl::describeCpuBudget(CpuBudget{24, 0, 2}) ==
          "online 24, affinity unrestricted, cgroup 2 -> 2 usable, limited by cgroup quota");
    CHECK(fl::describeCpuBudget(CpuBudget{}) ==
          "online unknown, affinity unrestricted, cgroup unlimited -> unknown usable, "
          "limited by nothing detectable");
}

TEST_CASE("parseCgroupV2CpuMax converts quota/period to whole CPUs", "[job][cpubudget]") {
    CHECK(fl::parseCgroupV2CpuMax("max 100000") == 0u); // unlimited
    CHECK(fl::parseCgroupV2CpuMax("100000 100000") == 1u);
    CHECK(fl::parseCgroupV2CpuMax("200000 100000") == 2u);
    // Fractional limits round UP: 1.5 CPUs is 2 usable, and rounding down would size a 0.5-CPU
    // pod's pool to zero.
    CHECK(fl::parseCgroupV2CpuMax("150000 100000") == 2u);
    CHECK(fl::parseCgroupV2CpuMax("50000 100000") == 1u);
    // A truncated line means the kernel's default 100 ms period.
    CHECK(fl::parseCgroupV2CpuMax("400000") == 4u);
    // Nothing readable is "no quota", never "no CPUs".
    CHECK(fl::parseCgroupV2CpuMax("") == 0u);
    CHECK(fl::parseCgroupV2CpuMax("garbage") == 0u);
    CHECK(fl::parseCgroupV2CpuMax("0 100000") == 0u);
}

TEST_CASE("parseCgroupV1CpuQuota reads the two-file spelling", "[job][cpubudget]") {
    CHECK(fl::parseCgroupV1CpuQuota("-1", "100000") == 0u); // v1 spells unlimited as -1
    CHECK(fl::parseCgroupV1CpuQuota("300000", "100000") == 3u);
    CHECK(fl::parseCgroupV1CpuQuota("120000", "100000") == 2u);
    CHECK(fl::parseCgroupV1CpuQuota("", "") == 0u);
    CHECK(fl::parseCgroupV1CpuQuota("100000", "0") == 0u); // a nonsense pair must not be acted on
}

TEST_CASE("cgroupV2QuotaAt walks to the tightest ancestor quota", "[job][cpubudget]") {
    // A quota that binds a process is not always written on its own cgroup: a container sees it at
    // the root of its namespace, a service inherits one from its systemd slice.
    const auto root = std::filesystem::temp_directory_path() / "fl-cgroup-walk-test";
    std::filesystem::remove_all(root); // a previous run that died mid-test must not seed this one
    std::filesystem::create_directories(root / "system.slice" / "fl-server.scope");
    const auto write = [](const std::filesystem::path& p, const char* text) { std::ofstream(p) << text << "\n"; };
    write(root / "cpu.max", "max 100000");
    write(root / "system.slice" / "cpu.max", "800000 100000");                     // 8 CPUs
    write(root / "system.slice" / "fl-server.scope" / "cpu.max", "200000 100000"); // 2 CPUs

    CHECK(fl::cgroupV2QuotaAt(root.string(), "/system.slice/fl-server.scope") == 2u);
    // The ancestor still binds when the leaf declares no quota of its own.
    write(root / "system.slice" / "fl-server.scope" / "cpu.max", "max 100000");
    CHECK(fl::cgroupV2QuotaAt(root.string(), "/system.slice/fl-server.scope") == 8u);
    // No quota anywhere on the path is unlimited, not zero CPUs.
    write(root / "system.slice" / "cpu.max", "max 100000");
    CHECK(fl::cgroupV2QuotaAt(root.string(), "/system.slice/fl-server.scope") == 0u);
    // A path that does not exist is simply unlimited -- never a throw on a startup path.
    CHECK(fl::cgroupV2QuotaAt(root.string(), "/no/such/cgroup") == 0u);
    std::filesystem::remove_all(root);
}

TEST_CASE("the pool sizes to the granted budget, and an explicit count still wins", "[job][cpubudget]") {
    // taskset -c 0-7 on this 24-CPU box: 7 background workers + the sim thread = 8, not 23.
    CHECK(JobSystem(0, CpuBudget{24, 8, 0}).workerCount() == 7u);
    // A pod with a 4-CPU limit on a 24-core node.
    CHECK(JobSystem(0, CpuBudget{24, 0, 4}).workerCount() == 3u);
    // Unconstrained, the behaviour is exactly what it was before #1380.
    CHECK(JobSystem(0, CpuBudget{24, 0, 0}).workerCount() == 23u);
    // An explicit sim_worker_threads is honoured verbatim -- it ignores the budget entirely, in
    // both directions, because the operator asking for it knows something the process does not.
    CHECK(JobSystem(1, CpuBudget{24, 8, 0}).workerCount() == 0u);
    CHECK(JobSystem(4, CpuBudget{24, 2, 2}).workerCount() == 3u);
    CHECK(JobSystem(16, CpuBudget{4, 0, 0}).workerCount() == 15u);
    // Nothing detectable falls back to 4 total, as it always has.
    CHECK(JobSystem(0, CpuBudget{}).workerCount() == 3u);
    // The budget is retained for the startup line rather than recomputed by whoever logs it.
    CHECK(JobSystem(0, CpuBudget{24, 8, 0}).cpuBudget().affinity == 8u);
}

TEST_CASE("the detected budget is self-consistent on this host", "[job][cpubudget]") {
    // Not an assertion about THIS machine's core count -- just that detection reports a coherent
    // budget rather than, say, a quota of zero that would silently serialize the sim.
    const CpuBudget b = fl::detectCpuBudget();
    const unsigned usable = fl::resolveCpuBudget(b);
    CHECK(usable >= 1u);
    if (b.online != 0u && b.affinity != 0u)
        CHECK(b.affinity <= b.online);
    CHECK(JobSystem(0, b).workerCount() == resolveWorkerCount(0, usable));
}
