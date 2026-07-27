// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ReplayFormat.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ReplayReader (#643) — reads a `.flrep` per docs/replay-format.md.
//
// It treats the file as HOSTILE INPUT, because it is: a replay is something a player downloads from
// a stranger. Every declared length is checked against what is actually left in the file before it is
// believed, decompressed chunk sizes are capped rather than honoured, and a malformed file produces a
// clear error and no partial state -- the fail-closed contract BitStream.h already provides for the
// records inside. fuzz/fuzz_flrep.cpp drives this class for that reason.
//
// Compatibility, per the spec: a newer MAJOR is refused outright with a message naming both versions,
// because a silent partial read that renders a plausible-but-wrong flight is worse than a refusal
// nobody can miss. A newer MINOR reads fine -- unknown sections are skipped by their declared length,
// which is what makes "additive is a minor bump" true rather than hopeful.
//
// A file whose trailer is missing or corrupt (an interrupted recording -- a crash, a killed server)
// is still played from the start: the index is rebuilt by forward scan, losing seek speed but not
// the recording.

namespace fl {

class ReplayReader {
  public:
    struct IndexEntry {
        uint64_t tick{0};   // keyframe tick
        uint64_t offset{0}; // byte offset of the chunk that starts with it
    };

    ReplayReader() = default;

    ReplayReader(const ReplayReader&) = delete;
    ReplayReader& operator=(const ReplayReader&) = delete;

    // Open, validate, read the header + sections, and load (or rebuild) the keyframe index.
    // False on any refusal, with the reason in lastError().
    bool open(const std::filesystem::path& path);
    void close();

    [[nodiscard]] bool isOpen() const noexcept {
        return m_in.is_open();
    }
    [[nodiscard]] const ReplayHeader& header() const noexcept {
        return m_header;
    }
    [[nodiscard]] const ReplaySections& sections() const noexcept {
        return m_sections;
    }
    [[nodiscard]] const std::vector<IndexEntry>& index() const noexcept {
        return m_index;
    }
    // True when the trailer was missing or unusable and the index came from a forward scan. Not an
    // error -- worth surfacing, because it means the recording was interrupted.
    [[nodiscard]] bool indexRebuilt() const noexcept {
        return m_indexRebuilt;
    }
    [[nodiscard]] uint64_t firstTick() const noexcept {
        return m_firstTick;
    }
    [[nodiscard]] uint64_t lastTick() const noexcept {
        return m_lastTick;
    }
    // Session length in seconds from the recorded tick rate -- never a hardcoded 60.
    [[nodiscard]] double durationSeconds() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept {
        return m_lastError;
    }

    // Read the next tick in order. False at end of stream or on the first malformed byte (check
    // lastError() to tell those apart: it is empty at a clean end).
    bool readNextTick(ReplayTick& out);

    // Position the cursor at the keyframe at or before `tick` -- the seek primitive scrubbing is
    // built from ("find the keyframe, decompress its chunk, roll forward"). Clamps to the first
    // keyframe for an earlier tick and to the last for a later one. False only if there is no index
    // at all (an empty recording).
    bool seekToKeyframeAtOrBefore(uint64_t tick);

    // Rewind to the first tick.
    bool rewind();

    // Decode one tick from a chunk payload buffer. Exposed so the writer's encode and this decode
    // stay each other's specification in tests, and so the fuzz harness can drive the tick decoder
    // directly rather than only through a whole file.
    [[nodiscard]] static bool decodeTick(const uint8_t* data, std::size_t size, std::size_t& cursor, ReplayTick& out);

  private:
    bool readSections(const std::vector<uint8_t>& head, std::size_t& cursor);
    bool loadTrailerIndex();
    bool rebuildIndexByScan();
    bool loadChunkAt(uint64_t offset);

    std::ifstream m_in;
    std::filesystem::path m_path;
    uint64_t m_fileSize{0};
    uint64_t m_bodyOffset{0}; // first chunk
    uint64_t m_bodyEnd{0};    // one past the last chunk (trailer start, or EOF when absent)

    ReplayHeader m_header;
    ReplaySections m_sections;
    std::vector<IndexEntry> m_index;
    bool m_indexRebuilt{false};
    uint64_t m_firstTick{0};
    uint64_t m_lastTick{0};

    std::vector<uint8_t> m_chunk; // decompressed payload of the loaded chunk
    std::size_t m_chunkCursor{0}; // read position within m_chunk
    uint32_t m_chunkTicksLeft{0};
    uint64_t m_nextChunkOffset{0};
    std::string m_lastError;
};

} // namespace fl
