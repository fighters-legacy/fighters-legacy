// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ReplayFormat.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ReplayWriter (#643) — writes a `.flrep` per docs/replay-format.md.
//
// Layout, in the order it hits the disk:
//
//   header + sections   (written once at open, so a reader populates its tables in ONE forward pass)
//   chunk, chunk, ...   (each = one keyframe tick + the delta ticks that follow it, zstd-framed)
//   index trailer       (keyframe tick -> byte offset; written last because the offsets are not
//                        known until the stream is complete, found first by a reader seeking to end)
//
// Chunking follows the keyframe cadence rather than a byte target because that is what makes a seek
// cost exactly one decompression: find the keyframe at or before the target, decompress its chunk,
// roll forward.
//
// A recording that is never closed (a crashed or killed server) has no trailer. That file is STILL
// PLAYABLE -- the reader rebuilds the index by forward scan. A replay of the session that crashed
// the server is exactly the replay someone most wants, so losing it to a missing 8-byte footer would
// be the wrong trade.
//
// All paths are std::filesystem::path end to end (the Windows-UTF-8 bug ConfigFile.h exists to
// avoid; FLIT's std::string ctor is the mistake not to repeat).

namespace fl {

class ReplayWriter {
  public:
    struct Config {
        std::filesystem::path dir;       // replay directory; created if absent
        std::string baseName{"session"}; // file stem; rotations append _001, _002, ...
        uint64_t maxFileBytes{256ull * 1024ull * 1024ull};
        // Bounds the DIRECTORY, not the session: a long-running server that recorded one file per
        // match would otherwise fill the disk one perfectly-rotated file at a time. Pruning only
        // ever deletes `.flrep` files in `dir`, oldest first, and never the file being written.
        uint32_t maxFiles{20};
    };

    ReplayWriter() = default;
    ~ReplayWriter();

    ReplayWriter(const ReplayWriter&) = delete;
    ReplayWriter& operator=(const ReplayWriter&) = delete;

    // Open the first file and write its header + sections. False on any filesystem error, with the
    // reason in lastError().
    bool open(const ReplayHeader& header, const ReplaySections& sections, const Config& cfg);

    // Append one tick. Chunks are flushed when the NEXT keyframe arrives, so a tick is not on disk
    // the instant this returns -- close() (or the next keyframe) puts it there.
    bool writeTick(const ReplayTick& tick);

    // Flush the pending chunk, write the index trailer, close the file. Idempotent.
    bool close();

    [[nodiscard]] bool isOpen() const noexcept {
        return m_out.is_open();
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }
    [[nodiscard]] uint64_t bytesWritten() const noexcept {
        return m_bytesWritten;
    }
    [[nodiscard]] uint64_t ticksWritten() const noexcept {
        return m_ticksWritten;
    }
    [[nodiscard]] const std::string& lastError() const noexcept {
        return m_lastError;
    }

    // Serialize one tick into the chunk-payload encoding. Exposed for the reader's round-trip tests
    // and for the fuzz seed minter -- the encoder and decoder are each other's specification.
    static void encodeTick(std::vector<uint8_t>& out, const ReplayTick& tick);

  private:
    bool openFile();     // opens m_path, writes header + sections
    bool flushChunk();   // compress + write the buffered chunk, record its index entry
    bool writeTrailer(); // index + extent + trailer offset
    bool rotate();       // close the current file, open the next, prune the directory
    void pruneDirectory();

    Config m_cfg;
    ReplayHeader m_header;
    ReplaySections m_sections;

    std::ofstream m_out;
    std::filesystem::path m_path;
    uint32_t m_fileSeq{0};

    std::vector<uint8_t> m_chunk;   // uncompressed payload of the chunk being built
    std::vector<uint8_t> m_scratch; // reused compression destination
    uint64_t m_chunkFirstTick{0};
    uint32_t m_chunkTickCount{0};

    struct IndexEntry {
        uint64_t tick{0};
        uint64_t offset{0};
    };
    std::vector<IndexEntry> m_index;

    uint64_t m_bytesWritten{0};
    uint64_t m_ticksWritten{0};
    uint64_t m_firstTick{0};
    uint64_t m_lastTick{0};
    bool m_haveAnyTick{false};
    std::string m_lastError;
};

} // namespace fl
