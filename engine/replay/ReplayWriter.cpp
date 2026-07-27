// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReplayWriter.h"

#include "net/ByteOrder.h"
#include "net/SnapshotCompression.h"

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <utility>

namespace fl {

using detail::putF64LE;
using detail::putStringLE;
using detail::putU16LE;
using detail::putU32LE;
using detail::putU64LE;
using detail::putU8;

namespace {

void putEvent(std::vector<uint8_t>& out, const MatchEvent& e) {
    putU64LE(out, e.seq);
    putU64LE(out, e.tick);
    putU8(out, static_cast<uint8_t>(e.type));
    putU32LE(out, e.subjectIdx);
    putU16LE(out, e.subjectGen);
    putU32LE(out, e.instigatorIdx);
    putU16LE(out, e.instigatorGen);
    putU32LE(out, e.actor);
    putU32LE(out, e.target);
    putU16LE(out, e.factionIndex);
    putU8(out, e.weaponClass);
    putU8(out, e.channel);
    putU32LE(out, static_cast<uint32_t>(e.value));
    // Attacker-controlled text (a chat line, an admin command). Stored length-prefixed and clamped;
    // every consumer that renders it still owes it escaping (WorldStateJson.h's rule).
    putStringLE(out, e.text.size() > kReplayMaxStringBytes ? std::string_view(e.text).substr(0, kReplayMaxStringBytes)
                                                           : std::string_view(e.text));
}

void putSection(std::vector<uint8_t>& out, ReplaySectionId id, const std::vector<uint8_t>& payload) {
    putU16LE(out, static_cast<uint16_t>(id));
    putU32LE(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

ReplayWriter::~ReplayWriter() {
    close();
}

void ReplayWriter::encodeTick(std::vector<uint8_t>& out, const ReplayTick& tick) {
    putU64LE(out, tick.tickIndex);
    putU16LE(out, tick.flags);
    putU16LE(out, tick.recordCount);
    const auto originCount = static_cast<uint16_t>(tick.origins.size() / 3);
    putU16LE(out, originCount);
    putU16LE(out, static_cast<uint16_t>(std::min<std::size_t>(tick.events.size(), 65535u)));
    for (uint16_t i = 0; i < originCount; ++i) {
        putF64LE(out, tick.origins[static_cast<std::size_t>(i) * 3 + 0]);
        putF64LE(out, tick.origins[static_cast<std::size_t>(i) * 3 + 1]);
        putF64LE(out, tick.origins[static_cast<std::size_t>(i) * 3 + 2]);
    }
    putU32LE(out, static_cast<uint32_t>(tick.records.size()));
    out.insert(out.end(), tick.records.begin(), tick.records.end());
    const std::size_t events = std::min<std::size_t>(tick.events.size(), 65535u);
    for (std::size_t i = 0; i < events; ++i)
        putEvent(out, tick.events[i]);
}

bool ReplayWriter::open(const ReplayHeader& header, const ReplaySections& sections, const Config& cfg) {
    close();

    m_cfg = cfg;
    m_header = header;
    m_sections = sections;
    m_fileSeq = 0;
    m_bytesWritten = 0;
    m_ticksWritten = 0;
    m_haveAnyTick = false;
    m_lastError.clear();

    std::error_code ec;
    std::filesystem::create_directories(m_cfg.dir, ec);
    if (ec) {
        m_lastError = "cannot create replay directory: " + ec.message();
        return false;
    }
    return openFile();
}

bool ReplayWriter::openFile() {
    std::filesystem::path p = m_cfg.dir;
    if (m_fileSeq == 0) {
        p /= m_cfg.baseName + ".flrep";
    } else {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%03u", m_fileSeq);
        p /= m_cfg.baseName + suffix + ".flrep";
    }
    m_path = p;

    m_out.open(m_path, std::ios::binary | std::ios::trunc);
    if (!m_out) {
        m_lastError = "cannot open replay file for writing: " + m_path.string();
        return false;
    }

    std::vector<uint8_t> head;
    head.insert(head.end(), std::begin(kReplayMagic), std::end(kReplayMagic));
    putU16LE(head, m_header.formatMajor);
    putU16LE(head, m_header.formatMinor);
    putStringLE(head, m_header.engineVersion);
    putU32LE(head, m_header.tickRateHz);
    putF64LE(head, m_header.planetRadiusM);
    putU64LE(head, m_header.startUnixSeconds);
    putStringLE(head, m_header.missionId);
    putU32LE(head, m_header.sessionFlags);
    putU32LE(head, m_header.keyframeIntervalTicks);

    // Sections, all before the frame stream so a reader fills its tables in one forward pass.
    std::vector<uint8_t> payload;
    payload.clear();
    putU32LE(payload, static_cast<uint32_t>(m_sections.entityTypes.size()));
    for (const ReplayEntityType& t : m_sections.entityTypes) {
        putU32LE(payload, t.typeIndex);
        putStringLE(payload, t.id);
        putStringLE(payload, t.name);
        putU8(payload, t.category);
        putU8(payload, t.projectileKind);
    }
    putSection(head, ReplaySectionId::EntityTypeManifest, payload);

    payload.clear();
    putU32LE(payload, static_cast<uint32_t>(m_sections.factions.size()));
    for (const ReplayFaction& f : m_sections.factions) {
        putU16LE(payload, f.factionIndex);
        putStringLE(payload, f.id);
        putStringLE(payload, f.name);
    }
    putSection(head, ReplaySectionId::FactionTable, payload);

    payload.clear();
    putU32LE(payload, static_cast<uint32_t>(m_sections.roster.size()));
    for (const ReplayRosterEntry& r : m_sections.roster) {
        putU32LE(payload, r.participantId);
        putStringLE(payload, r.callsign);
        putU16LE(payload, r.factionIndex);
        putU8(payload, r.role);
        putU8(payload, r.isBot ? 1u : 0u);
    }
    putSection(head, ReplaySectionId::Roster, payload);

    // Section list terminator: an id of 0 with no length. A reader stops here and starts the frame
    // stream, which is what lets the section list grow without the frame stream needing an offset.
    putU16LE(head, 0u);

    m_out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    if (!m_out) {
        m_lastError = "failed writing replay header";
        return false;
    }
    m_bytesWritten = head.size();
    m_chunk.clear();
    m_chunkTickCount = 0;
    m_index.clear();
    return true;
}

bool ReplayWriter::writeTick(const ReplayTick& tick) {
    if (!m_out.is_open())
        return false;

    // A chunk starts at a keyframe. The first tick of a recording is always one (the recorder forces
    // it), so a file never opens with deltas whose baseline is not in the file.
    if (tick.keyframe() && m_chunkTickCount > 0) {
        if (!flushChunk())
            return false;
        if (m_bytesWritten >= m_cfg.maxFileBytes && !rotate())
            return false;
    }

    if (m_chunkTickCount == 0)
        m_chunkFirstTick = tick.tickIndex;

    encodeTick(m_chunk, tick);
    ++m_chunkTickCount;
    ++m_ticksWritten;

    if (!m_haveAnyTick) {
        m_firstTick = tick.tickIndex;
        m_haveAnyTick = true;
    }
    m_lastTick = tick.tickIndex;
    return true;
}

bool ReplayWriter::flushChunk() {
    if (m_chunkTickCount == 0)
        return true;

    // zstd via engine-compress, the codec already documented deterministic for identical input.
    // compressSnapshotPayload returns 0 when compression does not win (tiny or incompressible), and
    // the chunk is then stored raw -- signalled by compressedBytes == 0, so a reader never has to
    // guess which it is holding.
    const std::size_t compressed = compressSnapshotPayload(m_chunk.data(), m_chunk.size(), m_scratch);

    std::vector<uint8_t> hdr;
    putU32LE(hdr, static_cast<uint32_t>(m_chunk.size()));
    putU32LE(hdr, static_cast<uint32_t>(compressed));
    putU64LE(hdr, m_chunkFirstTick);
    putU32LE(hdr, m_chunkTickCount);

    m_index.push_back({m_chunkFirstTick, m_bytesWritten});

    m_out.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
    if (compressed > 0)
        m_out.write(reinterpret_cast<const char*>(m_scratch.data()), static_cast<std::streamsize>(compressed));
    else
        m_out.write(reinterpret_cast<const char*>(m_chunk.data()), static_cast<std::streamsize>(m_chunk.size()));
    if (!m_out) {
        m_lastError = "failed writing replay chunk";
        return false;
    }

    m_bytesWritten += hdr.size() + (compressed > 0 ? compressed : m_chunk.size());
    m_chunk.clear();
    m_chunkTickCount = 0;
    return true;
}

bool ReplayWriter::writeTrailer() {
    std::vector<uint8_t> tail;
    putU32LE(tail, static_cast<uint32_t>(m_index.size()));
    for (const IndexEntry& e : m_index) {
        putU64LE(tail, e.tick);
        putU64LE(tail, e.offset);
    }
    // Extent, so a replay browser can show a duration without decompressing anything. A file with no
    // trailer reports its extent from the forward scan instead.
    putU64LE(tail, m_haveAnyTick ? m_firstTick : 0);
    putU64LE(tail, m_haveAnyTick ? m_lastTick : 0);
    const uint64_t trailerOffset = m_bytesWritten;
    putU64LE(tail, trailerOffset); // the last 8 bytes: where the table itself starts

    m_out.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!m_out) {
        m_lastError = "failed writing replay index trailer";
        return false;
    }
    m_bytesWritten += tail.size();
    return true;
}

bool ReplayWriter::rotate() {
    if (!flushChunk() || !writeTrailer())
        return false;
    m_out.close();

    ++m_fileSeq;
    m_bytesWritten = 0;
    if (!openFile())
        return false;
    // Prune AFTER the new file exists, so the count that decides what to delete includes it -- with
    // max_files = 1 the previous file is what goes, not nothing.
    pruneDirectory();
    return true;
}

void ReplayWriter::pruneDirectory() {
    if (m_cfg.maxFiles == 0)
        return;

    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::directory_iterator(m_cfg.dir, ec)) {
        if (ec)
            return;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".flrep")
            continue;
        const auto when = std::filesystem::last_write_time(entry.path(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        files.emplace_back(when, entry.path());
    }
    if (files.size() <= m_cfg.maxFiles)
        return;

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first < b.first;
        return a.second < b.second; // stable across filesystems with coarse timestamps
    });

    const std::size_t excess = files.size() - m_cfg.maxFiles;
    for (std::size_t i = 0; i < excess; ++i) {
        if (files[i].second == m_path)
            continue; // never the file being written
        std::filesystem::remove(files[i].second, ec);
        ec.clear();
    }
}

bool ReplayWriter::close() {
    if (!m_out.is_open())
        return true;

    bool ok = flushChunk();
    ok = writeTrailer() && ok;
    m_out.close();
    if (ok)
        pruneDirectory();
    return ok;
}

} // namespace fl
