// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

// RAII wrapper that creates a unique temporary directory on construction
// and removes it (recursively) on destruction.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        namespace fs = std::filesystem;
        auto tmp = fs::temp_directory_path();
        // Use a unique suffix based on the address of this object
        path = tmp / ("fl_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::string str() const {
        return path.string();
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};
