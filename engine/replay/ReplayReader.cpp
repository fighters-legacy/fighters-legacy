// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReplayReader.h"

#include "net/ByteOrder.h"
#include "net/SnapshotCompression.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace fl {

using detail::ByteCursor;

namespace {

// The fixed part of a chunk header: uncompressedBytes, compressedBytes, firstTick, tickCount.
constexpr std::size_t kChunkHeaderBytes = 4 + 4 + 8 + 4;

std::string versionText(uint16_t major, uint16_t minor) {
    return std::to_string(major) + "." + std::to_string(minor);
}

} // namespace

double ReplayReader::durationSeconds() const noexcept {
    if (m_header.tickRateHz == 0 || m_lastTick < m_firstTick)
        return 0.0;
    return static_cast<double>(m_lastTick - m_firstTick) / static_cast<double>(m_header.tickRateHz);
}

void ReplayReader::close() {
    m_in.close();
    m_index.clear();
    m_chunk.clear();
    m_chunkCursor = 0;
    m_chunkTicksLeft = 0;
    m_indexRebuilt = false;
    m_firstTick = 0;
    m_lastTick = 0;
}

bool ReplayReader::open(const std::filesystem::path& path) {
    close();
    m_lastError.clear();
    m_path = path;

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        m_lastError = "cannot stat replay file: " + ec.message();
        return false;
    }
    m_fileSize = static_cast<uint64_t>(size);

    m_in.open(path, std::ios::binary);
    if (!m_in) {
        m_lastError = "cannot open replay file: " + path.string();
        return false;
    }

    // The header + sections are bounded by construction (each section carries its length), but the
    // file is untrusted, so read a bounded prefix and parse it with a fail-closed cursor rather than
    // seeking around on declared offsets.
    const std::size_t prefixBytes =
        static_cast<std::size_t>(std::min<uint64_t>(m_fileSize, kReplayMaxSectionBytes + 4096u));
    std::vector<uint8_t> head(prefixBytes);
    m_in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(prefixBytes));
    if (static_cast<std::size_t>(m_in.gcount()) != prefixBytes) {
        m_lastError = "truncated replay header";
        close();
        return false;
    }

    ByteCursor c(head.data(), head.size());
    const uint8_t* magic = c.bytes(4);
    if (!magic || std::memcmp(magic, kReplayMagic, 4) != 0) {
        m_lastError = "not a replay file (bad magic)";
        close();
        return false;
    }

    m_header = ReplayHeader{};
    m_header.formatMajor = c.u16();
    m_header.formatMinor = c.u16();
    if (!c.ok()) {
        m_lastError = "truncated replay header";
        close();
        return false;
    }
    // The refusal the spec asks for by name: say what wrote it and what this build reads, so nobody
    // has to guess whether the file or the build is the problem.
    if (m_header.formatMajor > kReplayFormatMajor) {
        m_lastError = "this replay is format " + versionText(m_header.formatMajor, m_header.formatMinor) +
                      "; this build reads format " + std::to_string(kReplayFormatMajor) + ".x";
        close();
        return false;
    }

    m_header.engineVersion = c.str();
    m_header.tickRateHz = c.u32();
    m_header.planetRadiusM = c.f64();
    m_header.startUnixSeconds = c.u64();
    m_header.missionId = c.str();
    m_header.sessionFlags = c.u32();
    m_header.keyframeIntervalTicks = c.u32();
    if (!c.ok()) {
        m_lastError = "truncated replay header";
        close();
        return false;
    }
    if (m_header.tickRateHz == 0 || m_header.tickRateHz > 10000u) {
        m_lastError = "replay declares an impossible tick rate";
        close();
        return false;
    }

    std::size_t cursor = c.offset();
    if (!readSections(head, cursor)) {
        close();
        return false;
    }
    m_bodyOffset = cursor;
    m_bodyEnd = m_fileSize;

    if (!loadTrailerIndex()) {
        // Not an error: an interrupted recording has no usable trailer, and losing the whole file to
        // a missing 8-byte footer would be the wrong trade.
        if (!rebuildIndexByScan()) {
            close();
            return false;
        }
    }

    return rewind();
}

bool ReplayReader::readSections(const std::vector<uint8_t>& head, std::size_t& cursor) {
    ByteCursor c(head.data(), head.size());
    c.skip(cursor);

    for (;;) {
        const uint16_t id = c.u16();
        if (!c.ok()) {
            m_lastError = "truncated replay section list";
            return false;
        }
        if (id == 0) { // terminator
            cursor = c.offset();
            return true;
        }

        const uint32_t len = c.u32();
        if (!c.ok() || len > kReplayMaxSectionBytes) {
            m_lastError = "replay section declares an impossible length";
            return false;
        }
        const uint8_t* payload = c.bytes(len);
        if (!payload) {
            m_lastError = "replay section runs past the end of the file";
            return false;
        }

        ByteCursor s(payload, len);
        switch (static_cast<ReplaySectionId>(id)) {
        case ReplaySectionId::EntityTypeManifest: {
            const uint32_t n = s.u32();
            if (n > kReplayMaxEntityTypes) {
                m_lastError = "replay entity-type manifest declares an impossible count";
                return false;
            }
            for (uint32_t i = 0; i < n && s.ok(); ++i) {
                ReplayEntityType t;
                t.typeIndex = s.u32();
                t.id = s.str();
                t.name = s.str();
                t.category = s.u8();
                t.projectileKind = s.u8();
                if (s.ok())
                    m_sections.entityTypes.push_back(std::move(t));
            }
            break;
        }
        case ReplaySectionId::FactionTable: {
            const uint32_t n = s.u32();
            if (n > kReplayMaxFactions) {
                m_lastError = "replay faction table declares an impossible count";
                return false;
            }
            for (uint32_t i = 0; i < n && s.ok(); ++i) {
                ReplayFaction f;
                f.factionIndex = s.u16();
                f.id = s.str();
                f.name = s.str();
                if (s.ok())
                    m_sections.factions.push_back(std::move(f));
            }
            break;
        }
        case ReplaySectionId::Roster: {
            const uint32_t n = s.u32();
            if (n > kReplayMaxRosterEntries) {
                m_lastError = "replay roster declares an impossible count";
                return false;
            }
            for (uint32_t i = 0; i < n && s.ok(); ++i) {
                ReplayRosterEntry r;
                r.participantId = s.u32();
                r.callsign = s.str();
                r.factionIndex = s.u16();
                r.role = s.u8();
                r.isBot = s.u8() != 0u;
                if (s.ok())
                    m_sections.roster.push_back(std::move(r));
            }
            break;
        }
        default:
            // Unknown (or the reserved camera track): skipped by its declared length. This is the
            // mechanism that makes a newer MINOR readable instead of fatal.
            break;
        }

        if (!s.ok()) {
            m_lastError = "replay section is truncated";
            return false;
        }
    }
}

bool ReplayReader::loadTrailerIndex() {
    if (m_fileSize < m_bodyOffset + 8)
        return false;

    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(m_fileSize - 8), std::ios::beg);
    uint8_t tail[8]{};
    m_in.read(reinterpret_cast<char*>(tail), 8);
    if (m_in.gcount() != 8)
        return false;

    const uint64_t trailerOffset = detail::getU64LE(tail);
    // A trailer that starts before the frame stream or past the file is not a trailer.
    if (trailerOffset < m_bodyOffset || trailerOffset + 8 > m_fileSize)
        return false;

    const uint64_t trailerBytes = m_fileSize - trailerOffset;
    if (trailerBytes > kReplayMaxSectionBytes)
        return false;

    std::vector<uint8_t> buf(static_cast<std::size_t>(trailerBytes));
    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(trailerOffset), std::ios::beg);
    m_in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (static_cast<uint64_t>(m_in.gcount()) != trailerBytes)
        return false;

    ByteCursor c(buf.data(), buf.size());
    const uint32_t count = c.u32();
    // 16 bytes per entry, and the entries must fit in what the trailer actually occupies.
    if (!c.ok() || static_cast<uint64_t>(count) * 16ull + 4ull + 24ull > trailerBytes)
        return false;

    std::vector<IndexEntry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        IndexEntry e;
        e.tick = c.u64();
        e.offset = c.u64();
        if (!c.ok() || e.offset < m_bodyOffset || e.offset + kChunkHeaderBytes > trailerOffset)
            return false;
        entries.push_back(e);
    }
    const uint64_t first = c.u64();
    const uint64_t last = c.u64();
    if (!c.ok())
        return false;

    // Offsets must be strictly increasing: a hostile trailer must not be able to make a seek loop.
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].offset <= entries[i - 1].offset || entries[i].tick <= entries[i - 1].tick)
            return false;
    }

    m_index = std::move(entries);
    m_firstTick = first;
    m_lastTick = last;
    m_bodyEnd = trailerOffset;
    m_indexRebuilt = false;
    // A zero-entry index is legitimate: a session that recorded no ticks still produced a valid,
    // closed file, and refusing it would report a corrupt replay where there is merely an empty one.
    return true;
}

bool ReplayReader::rebuildIndexByScan() {
    m_index.clear();
    m_indexRebuilt = true;
    m_bodyEnd = m_fileSize;

    uint64_t offset = m_bodyOffset;
    bool haveExtent = false;
    while (offset + kChunkHeaderBytes <= m_fileSize) {
        uint8_t hdr[kChunkHeaderBytes]{};
        m_in.clear();
        m_in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_in.read(reinterpret_cast<char*>(hdr), static_cast<std::streamsize>(kChunkHeaderBytes));
        if (static_cast<std::size_t>(m_in.gcount()) != kChunkHeaderBytes)
            break;

        const uint32_t rawBytes = detail::getU32LE(hdr);
        const uint32_t compBytes = detail::getU32LE(hdr + 4);
        const uint64_t firstTick = detail::getU64LE(hdr + 8);
        const uint32_t tickCount = detail::getU32LE(hdr + 16);

        if (rawBytes == 0 || rawBytes > kReplayMaxChunkBytes || compBytes > kReplayMaxChunkBytes || tickCount == 0 ||
            tickCount > kReplayMaxTicksPerChunk)
            break; // this is the trailer, or garbage; either way the frame stream ends here

        const uint64_t payload = compBytes > 0 ? compBytes : rawBytes;
        if (offset + kChunkHeaderBytes + payload > m_fileSize)
            break; // a truncated final chunk: keep everything before it

        m_index.push_back({firstTick, offset});
        if (!haveExtent) {
            m_firstTick = firstTick;
            haveExtent = true;
        }
        m_lastTick = firstTick + tickCount - 1; // exact for a contiguous recording, which ours is
        m_bodyEnd = offset + kChunkHeaderBytes + payload;
        offset = m_bodyEnd;
    }

    if (m_index.empty()) {
        m_lastError = "replay contains no readable frame data";
        return false;
    }
    return true;
}

bool ReplayReader::rewind() {
    if (m_index.empty()) {
        // An empty (but valid) recording: nothing to read, and readNextTick says so immediately.
        m_chunk.clear();
        m_chunkCursor = 0;
        m_chunkTicksLeft = 0;
        m_nextChunkOffset = m_bodyEnd;
        return true;
    }
    return loadChunkAt(m_index.front().offset);
}

bool ReplayReader::seekToKeyframeAtOrBefore(uint64_t tick) {
    if (m_index.empty())
        return false;

    std::size_t pick = 0;
    for (std::size_t i = 0; i < m_index.size(); ++i) {
        if (m_index[i].tick <= tick)
            pick = i;
        else
            break;
    }
    return loadChunkAt(m_index[pick].offset);
}

bool ReplayReader::loadChunkAt(uint64_t offset) {
    m_chunk.clear();
    m_chunkCursor = 0;
    m_chunkTicksLeft = 0;

    if (offset + kChunkHeaderBytes > m_fileSize) {
        m_lastError = "replay chunk offset runs past the end of the file";
        return false;
    }

    uint8_t hdr[kChunkHeaderBytes]{};
    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    m_in.read(reinterpret_cast<char*>(hdr), static_cast<std::streamsize>(kChunkHeaderBytes));
    if (static_cast<std::size_t>(m_in.gcount()) != kChunkHeaderBytes) {
        m_lastError = "truncated replay chunk header";
        return false;
    }

    const uint32_t rawBytes = detail::getU32LE(hdr);
    const uint32_t compBytes = detail::getU32LE(hdr + 4);
    const uint32_t tickCount = detail::getU32LE(hdr + 16);

    // Caps, not claims: a frame declaring a huge decompressed size is rejected outright rather than
    // honoured with an allocation.
    if (rawBytes == 0 || rawBytes > kReplayMaxChunkBytes || compBytes > kReplayMaxChunkBytes || tickCount == 0 ||
        tickCount > kReplayMaxTicksPerChunk) {
        m_lastError = "replay chunk declares impossible sizes";
        return false;
    }

    const uint64_t payloadBytes = compBytes > 0 ? compBytes : rawBytes;
    if (offset + kChunkHeaderBytes + payloadBytes > m_fileSize) {
        m_lastError = "replay chunk runs past the end of the file";
        return false;
    }

    std::vector<uint8_t> payload(static_cast<std::size_t>(payloadBytes));
    m_in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payloadBytes));
    if (static_cast<uint64_t>(m_in.gcount()) != payloadBytes) {
        m_lastError = "truncated replay chunk";
        return false;
    }

    if (compBytes > 0) {
        if (!decompressSnapshotPayload(payload.data(), payload.size(), rawBytes, m_chunk)) {
            m_lastError = "replay chunk failed to decompress";
            return false;
        }
    } else {
        m_chunk = std::move(payload);
    }

    m_chunkTicksLeft = tickCount;
    m_nextChunkOffset = offset + kChunkHeaderBytes + payloadBytes;
    return true;
}

bool ReplayReader::decodeTick(const uint8_t* data, std::size_t size, std::size_t& cursor, ReplayTick& out) {
    ByteCursor c(data, size);
    c.skip(cursor);

    out = ReplayTick{};
    out.tickIndex = c.u64();
    out.flags = c.u16();
    out.recordCount = c.u16();
    const uint16_t originCount = c.u16();
    const uint16_t eventCount = c.u16();
    if (!c.ok())
        return false;

    out.origins.resize(static_cast<std::size_t>(originCount) * 3);
    for (std::size_t i = 0; i < out.origins.size(); ++i)
        out.origins[i] = c.f64();
    if (!c.ok())
        return false;

    const uint32_t recordBytes = c.u32();
    // Bounds-check against what is actually left before reserving: a recordCount or a byte length is
    // never trusted enough to allocate on.
    const uint8_t* records = c.bytes(recordBytes);
    if (!records)
        return false;
    out.records.assign(records, records + recordBytes);

    out.events.reserve(std::min<std::size_t>(eventCount, 256u));
    for (uint16_t i = 0; i < eventCount; ++i) {
        MatchEvent e;
        e.seq = c.u64();
        e.tick = c.u64();
        const uint8_t type = c.u8();
        if (!c.ok())
            return false;
        // Gate the ordinal before the cast -- it came from a file a stranger wrote.
        e.type = isMatchEventTypeOrdinal(type) ? static_cast<MatchEventType>(type) : MatchEventType::Spawn;
        e.subjectIdx = c.u32();
        e.subjectGen = c.u16();
        e.instigatorIdx = c.u32();
        e.instigatorGen = c.u16();
        e.actor = c.u32();
        e.target = c.u32();
        e.factionIndex = c.u16();
        e.weaponClass = c.u8();
        e.channel = c.u8();
        e.value = static_cast<int32_t>(c.u32());
        e.text = c.str();
        if (!c.ok())
            return false;
        out.events.push_back(std::move(e));
    }

    cursor = c.offset();
    return true;
}

bool ReplayReader::readNextTick(ReplayTick& out) {
    if (!m_in.is_open())
        return false;

    if (m_chunkTicksLeft == 0) {
        if (m_nextChunkOffset + kChunkHeaderBytes > m_bodyEnd)
            return false; // clean end of stream: lastError stays empty
        if (!loadChunkAt(m_nextChunkOffset))
            return false;
    }

    if (!decodeTick(m_chunk.data(), m_chunk.size(), m_chunkCursor, out)) {
        m_lastError = "malformed tick record in replay chunk";
        m_chunkTicksLeft = 0;
        return false;
    }
    --m_chunkTicksLeft;
    if (m_chunkTicksLeft == 0)
        m_chunkCursor = 0; // next chunk starts a fresh buffer
    return true;
}

} // namespace fl
