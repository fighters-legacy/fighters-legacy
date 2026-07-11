// SPDX-License-Identifier: GPL-3.0-or-later
//
// Round-trip + malformed-input tests for the FLIT input-trace codec (issue #560): the
// InputTraceWriter (via an injected in-memory stream) and the pure parseInputTrace reader.
#include <net/InputTraceReader.h>
#include <net/InputTraceWriter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using namespace fl;

namespace {
std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
} // namespace

TEST_CASE("InputTraceWriter -> parseInputTrace round-trips header and records", "[input_trace]") {
    std::ostringstream os;
    {
        InputTraceWriter w(os, /*tickRate=*/60u);
        REQUIRE(w.good());
        w.writeRecord(100, 0.5f, -0.25f, 0.75f, -1.0f, 0x03u);
        w.writeRecord(101, 1.0f, 0.0f, -0.5f, 0.5f, 0x00u);
        w.writeRecord(102, 0.0f, 1.0f, 1.0f, 0.0f, 0x02u);
    }
    const std::string blob = os.str();
    CHECK(blob.size() == kInputTraceHeaderBytes + 3 * kInputTraceRecordBytes);

    const auto buf = bytesOf(blob);
    InputTrace tr;
    std::string err;
    REQUIRE(parseInputTrace(buf.data(), buf.size(), tr, err));
    CHECK(err.empty());
    CHECK(tr.tickRate == 60u);
    REQUIRE(tr.records.size() == 3);

    CHECK(tr.records[0].serverTick == 100u);
    CHECK(tr.records[0].throttle == Catch::Approx(0.5f));
    CHECK(tr.records[0].elevator == Catch::Approx(-0.25f));
    CHECK(tr.records[0].aileron == Catch::Approx(0.75f));
    CHECK(tr.records[0].rudder == Catch::Approx(-1.0f));
    CHECK(tr.records[0].buttons == 0x03u);

    CHECK(tr.records[2].serverTick == 102u);
    CHECK(tr.records[2].elevator == Catch::Approx(1.0f));
    CHECK(tr.records[2].buttons == 0x02u);
}

TEST_CASE("empty trace (header only) parses to zero records", "[input_trace]") {
    std::ostringstream os;
    {
        InputTraceWriter w(os, 30u);
    }
    const auto buf = bytesOf(os.str());
    InputTrace tr;
    std::string err;
    REQUIRE(parseInputTrace(buf.data(), buf.size(), tr, err));
    CHECK(tr.tickRate == 30u);
    CHECK(tr.records.empty());
}

TEST_CASE("parseInputTrace rejects malformed input", "[input_trace]") {
    InputTrace tr;
    std::string err;

    SECTION("too small for a header") {
        std::vector<uint8_t> buf{'F', 'L', 'I'};
        CHECK_FALSE(parseInputTrace(buf.data(), buf.size(), tr, err));
        CHECK_FALSE(err.empty());
    }
    SECTION("bad magic") {
        std::vector<uint8_t> buf(kInputTraceHeaderBytes, 0);
        buf[0] = 'X';
        CHECK_FALSE(parseInputTrace(buf.data(), buf.size(), tr, err));
    }
    SECTION("unsupported version") {
        std::ostringstream os;
        {
            InputTraceWriter w(os, 60u);
        }
        auto buf = bytesOf(os.str());
        buf[4] = 0xFF; // corrupt the version LSB
        CHECK_FALSE(parseInputTrace(buf.data(), buf.size(), tr, err));
    }
    SECTION("partial record (body not a whole number of records)") {
        std::ostringstream os;
        {
            InputTraceWriter w(os, 60u);
            w.writeRecord(1, 0.f, 0.f, 0.f, 0.f, 0u);
        }
        auto buf = bytesOf(os.str());
        buf.pop_back(); // drop one byte of the record
        CHECK_FALSE(parseInputTrace(buf.data(), buf.size(), tr, err));
    }
}
