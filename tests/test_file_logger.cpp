// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "FileLogger.h"
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

TEST_CASE("FileLogger: open creates log directory if absent", "[file_logger]") {
    TempDir tmp;
    auto logDir = (tmp.path / "logs").string();
    FileLogger logger;
    REQUIRE(logger.open(logDir, 10));
    REQUIRE(fs::exists(logDir));
    REQUIRE(logger.isOpen());
}

TEST_CASE("FileLogger: isOpen false before open and after close", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    CHECK_FALSE(logger.isOpen());
    REQUIRE(logger.open(tmp.str(), 10));
    CHECK(logger.isOpen());
    logger.close();
    CHECK_FALSE(logger.isOpen());
}

TEST_CASE("FileLogger: log below minLevel is suppressed", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.setMinLevel(LogLevel::Warn);
    logger.log(LogLevel::Debug, "f.cpp", 1, "debug msg");
    logger.log(LogLevel::Info, "f.cpp", 2, "info msg");
    logger.flush();

    // Ring should have 0 entries (both suppressed)
    FileLogger::RingEntry buf[10];
    CHECK(logger.copyLastLines(buf, 10) == 0);
}

TEST_CASE("FileLogger: log at or above minLevel is written", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.setMinLevel(LogLevel::Warn);
    logger.log(LogLevel::Warn, "f.cpp", 3, "warn msg");
    logger.log(LogLevel::Error, "f.cpp", 4, "error msg");

    FileLogger::RingEntry buf[10];
    int n = logger.copyLastLines(buf, 10);
    REQUIRE(n == 2);
    CHECK(buf[0].level == LogLevel::Warn);
    CHECK(buf[1].level == LogLevel::Error);
}

TEST_CASE("FileLogger: setMinLevel changes level mid-session", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.setMinLevel(LogLevel::Error);
    logger.log(LogLevel::Info, "f.cpp", 1, "suppressed");
    logger.setMinLevel(LogLevel::Info);
    logger.log(LogLevel::Info, "f.cpp", 2, "written");

    FileLogger::RingEntry buf[10];
    int n = logger.copyLastLines(buf, 10);
    REQUIRE(n == 1);
    CHECK(std::string(buf[0].message) == "written");
}

TEST_CASE("FileLogger: ring buffer wraps and returns 200 most recent", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.setMinLevel(LogLevel::Debug);

    for (int i = 0; i < 250; ++i)
        logger.log(LogLevel::Info, "f.cpp", i, ("msg " + std::to_string(i)).c_str());

    FileLogger::RingEntry buf[200];
    int n = logger.copyLastLines(buf, 200);
    REQUIRE(n == 200);
    // Most recent 200 = messages 50..249
    CHECK(std::string(buf[0].message) == "msg 50");
    CHECK(std::string(buf[199].message) == "msg 249");
}

TEST_CASE("FileLogger: copyLastLines with fewer than requested entries", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.log(LogLevel::Info, "f.cpp", 1, "only");

    FileLogger::RingEntry buf[200];
    int n = logger.copyLastLines(buf, 200);
    CHECK(n == 1);
}

TEST_CASE("FileLogger: copyLastLines(0) returns 0", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.log(LogLevel::Info, "f.cpp", 1, "msg");

    FileLogger::RingEntry buf[10];
    CHECK(logger.copyLastLines(buf, 0) == 0);
}

TEST_CASE("FileLogger: retention keeps at most maxRetained files", "[file_logger]") {
    TempDir tmp;
    // Seed 12 old engine_*.log files
    for (int i = 0; i < 12; ++i) {
        std::ofstream f((tmp.path / ("engine_2026-01-0" + std::to_string(i) + "_00-00-00.log")).string());
        f << "old\n";
    }
    // open() with maxRetained=10 should leave exactly 10 (11 old + 1 new = 12 → delete 3 oldest)
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));

    int count = 0;
    for (auto& entry : fs::directory_iterator(tmp.path)) {
        auto name = entry.path().filename().string();
        if (name.rfind("engine_", 0) == 0 && entry.path().extension() == ".log")
            ++count;
    }
    CHECK(count == 10);
}

TEST_CASE("FileLogger: flush does not crash on open logger", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.log(LogLevel::Info, "f.cpp", 1, "msg");
    REQUIRE_NOTHROW(logger.flush());
}

TEST_CASE("FileLogger: currentLogPath returns non-empty engine_ filename", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    auto& p = logger.currentLogPath();
    CHECK_FALSE(p.empty());
    CHECK(fs::path(p).filename().string().rfind("engine_", 0) == 0);
}

TEST_CASE("FileLogger: log writes formatted line to file", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.log(LogLevel::Info, "engine/Foo.cpp", 42, "hello world");
    logger.flush();

    std::ifstream f(logger.currentLogPath());
    std::string line;
    std::getline(f, line);
    CHECK(line.find("[INFO ]") != std::string::npos);
    CHECK(line.find("engine/Foo.cpp") != std::string::npos);
    CHECK(line.find("42") != std::string::npos);
    CHECK(line.find("hello world") != std::string::npos);
}

TEST_CASE("FileLogger: log from multiple threads does not crash", "[file_logger]") {
    TempDir tmp;
    FileLogger logger;
    REQUIRE(logger.open(tmp.str(), 10));
    logger.setMinLevel(LogLevel::Debug);

    auto worker = [&](int id) {
        for (int i = 0; i < 50; ++i)
            logger.log(LogLevel::Info, "t.cpp", i, ("thread " + std::to_string(id)).c_str());
    };

    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    std::thread t3(worker, 3);
    t1.join();
    t2.join();
    t3.join();

    FileLogger::RingEntry buf[200];
    int n = logger.copyLastLines(buf, 200);
    CHECK(n > 0);
}
