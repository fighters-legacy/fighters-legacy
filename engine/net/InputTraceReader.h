// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputTraceReader — parses a "FLIT" trace (see InputTraceFormat.h) into memory.
//
// parseInputTrace() is pure (operates on a byte buffer, no I/O) so it unit-tests directly and
// so the malformed-input paths are covered without fixtures on disk. loadInputTraceFile() is a
// thin convenience that slurps a file and delegates. The bot_swarm harness loads a trace once
// and shares the immutable result across all synthetic clients (TracePattern).

#include "InputTraceFormat.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace fl {

struct InputTrace {
    uint32_t tickRate{0};
    std::vector<InputTraceRecord> records;
};

// Parses `size` bytes at `data`. Returns true and fills `out` on success; on failure returns
// false and sets `err` to a human-readable reason (out is left cleared). A header with zero
// records is valid (an empty trace).
inline bool parseInputTrace(const uint8_t* data, std::size_t size, InputTrace& out, std::string& err) {
    out = InputTrace{};
    if (size < kInputTraceHeaderBytes) {
        err = "trace too small for header";
        return false;
    }
    if (std::memcmp(data, kInputTraceMagic, sizeof(kInputTraceMagic)) != 0) {
        err = "bad magic (not a FLIT trace)";
        return false;
    }
    const uint16_t version = detail::getU16LE(data + 4);
    if (version != kInputTraceVersion) {
        err = "unsupported trace version " + std::to_string(version);
        return false;
    }
    const uint32_t tickRate = detail::getU32LE(data + 6);
    const std::size_t bodyBytes = size - kInputTraceHeaderBytes;
    if (bodyBytes % kInputTraceRecordBytes != 0) {
        err = "trace body is not a whole number of records";
        return false;
    }
    out.tickRate = tickRate;
    const std::size_t count = bodyBytes / kInputTraceRecordBytes;
    out.records.reserve(count);
    const uint8_t* p = data + kInputTraceHeaderBytes;
    for (std::size_t i = 0; i < count; ++i, p += kInputTraceRecordBytes) {
        InputTraceRecord r;
        r.serverTick = detail::getU64LE(p);
        r.throttle = detail::getF32LE(p + 8);
        r.elevator = detail::getF32LE(p + 12);
        r.aileron = detail::getF32LE(p + 16);
        r.rudder = detail::getF32LE(p + 20);
        r.buttons = detail::getU32LE(p + 24);
        r.flaps = p[28];
        r.speedbrake = p[29];
        r.artButtons = p[30];
        out.records.push_back(r);
    }
    return true;
}

// Reads the whole file at `path` and parses it. Returns false with `err` set if the file cannot
// be opened or the contents are malformed.
inline bool loadInputTraceFile(const std::string& path, InputTrace& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "could not open trace file: " + path;
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseInputTrace(buf.data(), buf.size(), out, err);
}

} // namespace fl
