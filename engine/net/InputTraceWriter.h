// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputTraceWriter — appends accepted MsgClientInput samples to a "FLIT" trace stream.
//
// Header-only. Two constructors: one takes an injected std::ostream (not owned) so unit tests
// can round-trip through an in-memory std::ostringstream without touching the filesystem; the
// other opens a binary file at a path. good() reflects whether the target stream is writable.
// WorldBroadcaster owns one writer per peer while server-side input tracing is enabled (#560);
// records are written on the sim thread only (the ENet host is sim-thread-owned), so no locking.
//
// Format: see InputTraceFormat.h.

#include "InputTraceFormat.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace fl {

class InputTraceWriter {
  public:
    // Write to an injected stream (not owned). The header is emitted immediately.
    InputTraceWriter(std::ostream& os, uint32_t tickRate) : m_os(&os) {
        writeHeader(tickRate);
    }

    // Open `path` for binary writing (truncating). good() is false if the file could not be
    // opened; the caller should skip recording in that case.
    //
    // std::filesystem::path, not std::string (#643): on Windows a std::string path is interpreted in
    // the active code page, so a player whose profile directory contains non-ASCII cannot write a
    // trace at all. This is the bug engine/config/ConfigFile.h exists to avoid, and the one
    // docs/replay-format.md names as the mistake `.flrep` must not repeat.
    InputTraceWriter(const std::filesystem::path& path, uint32_t tickRate)
        : m_file(std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc)) {
        if (m_file->good()) {
            m_os = m_file.get();
            writeHeader(tickRate);
        }
    }

    InputTraceWriter(const InputTraceWriter&) = delete;
    InputTraceWriter& operator=(const InputTraceWriter&) = delete;

    bool good() const {
        return m_os != nullptr && m_os->good();
    }

    // Append one 32-byte record. No-op if the stream is not writable.
    void writeRecord(uint64_t serverTick, float throttle, float elevator, float aileron, float rudder, uint32_t buttons,
                     uint8_t flaps = 0, uint8_t speedbrake = 0, uint8_t artButtons = 0) {
        if (!good())
            return;
        std::vector<uint8_t> rec;
        rec.reserve(kInputTraceRecordBytes);
        detail::putU64LE(rec, serverTick);
        detail::putF32LE(rec, throttle);
        detail::putF32LE(rec, elevator);
        detail::putF32LE(rec, aileron);
        detail::putF32LE(rec, rudder);
        detail::putU32LE(rec, buttons);
        rec.push_back(flaps);
        rec.push_back(speedbrake);
        rec.push_back(artButtons);
        rec.push_back(0); // reserved
        m_os->write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
    }

  private:
    void writeHeader(uint32_t tickRate) {
        if (m_os == nullptr)
            return;
        std::vector<uint8_t> hdr;
        hdr.reserve(kInputTraceHeaderBytes);
        for (char c : kInputTraceMagic)
            hdr.push_back(static_cast<uint8_t>(c));
        detail::putU16LE(hdr, kInputTraceVersion);
        detail::putU32LE(hdr, tickRate);
        m_os->write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
    }

    std::unique_ptr<std::ofstream> m_file; // owns the file when the path constructor is used
    std::ostream* m_os{nullptr};           // active sink (file or injected); null = not writable
};

} // namespace fl
