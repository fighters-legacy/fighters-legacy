// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>

// Unique temporary paths for tests (#787).
//
// A test may not assume it is the only process on the machine. `catch_discover_tests` registers each
// TEST_CASE as its own ctest test, so under `ctest -j` the cases of a single binary run as CONCURRENT
// PROCESSES — and anything a test names by a fixed string (a port, a temp directory) is therefore
// shared with a sibling that is running right now.
//
// A per-process counter does NOT solve this, and that is the trap this header exists to close: the
// counter restarts at 1 in every process, so every process building "fl_lic_test_" + counter claims
// the SAME directory, and the first one to finish deletes it out from under the others. That is a
// real failure we hit (`test_validate_licenses` under a parallel ASan run), and it reads as a
// mysterious flake rather than as what it is.
//
// So the name is salted with a token drawn once per process from std::random_device, and then a
// counter for uniqueness within the process.

namespace fl::test {

// A random token, generated once per process. Not for security — only to make a name that no other
// process on this machine is using.
inline const std::string& processToken() {
    static const std::string token = [] {
        std::random_device rd;
        std::uniform_int_distribution<uint64_t> dist;
        std::mt19937_64 gen(rd());
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(gen)));
        return std::string(buf);
    }();
    return token;
}

// A path under the system temp dir that is unique across processes AND within this process.
// Does not create anything — the caller decides whether it wants a file or a directory.
inline std::filesystem::path uniqueTempPath(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path() / (prefix + "_" + processToken() + "_" + std::to_string(n));
}

// Creates the directory and removes it (recursively, best-effort) on destruction.
class TempDirGuard {
  public:
    explicit TempDirGuard(const std::string& prefix) : m_path(uniqueTempPath(prefix)) {
        std::filesystem::create_directories(m_path);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec); // best-effort; a failed cleanup must not fail a test
    }

    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

} // namespace fl::test
