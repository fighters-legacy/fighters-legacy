// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportIndexFile.h"
#include "math/Fnv.h"
#include "net/ByteOrder.h" // the one little-endian codec (#1255)

#include <bit>
#include <cstring>
#include <string>

namespace fl {

namespace {

constexpr uint32_t kMagic = 0x42414C46u; // "FLAB" little-endian
constexpr uint32_t kVersion = 1u;
constexpr std::size_t kHeaderBytes = 28;
constexpr std::size_t kAirportRecordBytes = 58;
constexpr std::size_t kRunwayRecordBytes = 13;

} // namespace

uint64_t airportSourceHash(std::string_view airportsCsv, std::string_view runwaysCsv) noexcept {
    // One running hash over both files, not two hashes combined: either CSV changing has to change
    // the result, and concatenating them into one fold is what makes that true.
    return fnv1a64(runwaysCsv, fnv1a64(airportsCsv));
}

std::vector<uint8_t> writeAirportIndex(const std::vector<AirportDef>& defs, uint64_t sourceHash) {
    // String blob: all ids in order, then all names in order. Offsets are relative to the blob start.
    std::string blob;
    struct StrRef {
        uint32_t ofs;
        uint16_t len;
    };
    std::vector<StrRef> idRefs, nameRefs;
    idRefs.reserve(defs.size());
    nameRefs.reserve(defs.size());
    for (const auto& d : defs) {
        idRefs.push_back({static_cast<uint32_t>(blob.size()), static_cast<uint16_t>(d.id.size())});
        blob += d.id;
    }
    for (const auto& d : defs) {
        nameRefs.push_back({static_cast<uint32_t>(blob.size()), static_cast<uint16_t>(d.name.size())});
        blob += d.name;
    }

    uint32_t runwayTotal = 0;
    for (const auto& d : defs)
        runwayTotal += static_cast<uint32_t>(d.runways.size());

    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + defs.size() * kAirportRecordBytes + runwayTotal * kRunwayRecordBytes + blob.size());

    detail::putU32LE(out, kMagic);
    detail::putU32LE(out, kVersion);
    detail::putU64LE(out, sourceHash);
    detail::putU32LE(out, static_cast<uint32_t>(defs.size()));
    detail::putU32LE(out, runwayTotal);
    detail::putU32LE(out, static_cast<uint32_t>(blob.size()));

    for (std::size_t i = 0; i < defs.size(); ++i) {
        const AirportDef& d = defs[i];
        detail::putU32LE(out, idRefs[i].ofs);
        detail::putU16LE(out, idRefs[i].len);
        detail::putU32LE(out, nameRefs[i].ofs);
        detail::putU16LE(out, nameRefs[i].len);
        detail::putF64LE(out, d.latRad);
        detail::putF64LE(out, d.lonRad);
        detail::putF64LE(out, d.elevationM);
        uint8_t flags = 0;
        if (d.acceptsLandings)
            flags |= 0x01u;
        if (d.useWorldXZ)
            flags |= 0x02u;
        out.push_back(flags);
        out.push_back(0); // pad
        detail::putF64LE(out, d.worldX);
        detail::putF64LE(out, d.worldZ);
        detail::putU16LE(out, static_cast<uint16_t>(d.runways.size()));
        detail::putU16LE(out, 0); // pad
    }

    for (const auto& d : defs) {
        for (const RunwayDef& rw : d.runways) {
            detail::putF32LE(out, rw.headingDeg);
            detail::putF32LE(out, rw.lengthM);
            detail::putF32LE(out, rw.widthM);
            out.push_back(static_cast<uint8_t>(rw.surface));
        }
    }

    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

std::optional<std::vector<AirportDef>> readAirportIndex(std::span<const uint8_t> bytes, uint64_t expectedSourceHash) {
    detail::ByteCursor r(bytes.data(), bytes.size());
    if (r.u32() != kMagic || r.u32() != kVersion)
        return std::nullopt;
    if (r.u64() != expectedSourceHash)
        return std::nullopt;
    const uint32_t airportCount = r.u32();
    const uint32_t runwayTotal = r.u32();
    const uint32_t blobSize = r.u32();
    if (!r.ok())
        return std::nullopt;

    // Bounds: the buffer must hold the header, all fixed records, and the string blob exactly.
    const std::size_t expected = kHeaderBytes + static_cast<std::size_t>(airportCount) * kAirportRecordBytes +
                                 static_cast<std::size_t>(runwayTotal) * kRunwayRecordBytes + blobSize;
    if (bytes.size() != expected)
        return std::nullopt;

    struct Rec {
        uint32_t idOfs, nameOfs;
        uint16_t idLen, nameLen, runwayCount;
        double latRad, lonRad, elevationM, worldX, worldZ;
        uint8_t flags;
    };
    std::vector<Rec> recs(airportCount);
    for (uint32_t i = 0; i < airportCount; ++i) {
        Rec& rec = recs[i];
        rec.idOfs = r.u32();
        rec.idLen = r.u16();
        rec.nameOfs = r.u32();
        rec.nameLen = r.u16();
        rec.latRad = r.f64();
        rec.lonRad = r.f64();
        rec.elevationM = r.f64();
        rec.flags = r.u8();
        (void)r.u8(); // pad
        rec.worldX = r.f64();
        rec.worldZ = r.f64();
        rec.runwayCount = r.u16();
        (void)r.u16(); // pad
    }

    struct RwRec {
        float heading, length, width;
        uint8_t surface;
    };
    std::vector<RwRec> rws(runwayTotal);
    for (uint32_t i = 0; i < runwayTotal; ++i) {
        rws[i].heading = r.f32();
        rws[i].length = r.f32();
        rws[i].width = r.f32();
        rws[i].surface = r.u8();
    }
    if (!r.ok())
        return std::nullopt;

    const std::size_t blobStart = r.offset();
    auto str = [&](uint32_t ofs, uint16_t len) -> std::optional<std::string> {
        if (static_cast<std::size_t>(ofs) + len > blobSize)
            return std::nullopt;
        return std::string(reinterpret_cast<const char*>(bytes.data()) + blobStart + ofs, len);
    };

    std::vector<AirportDef> defs(airportCount);
    uint32_t rwCursor = 0;
    for (uint32_t i = 0; i < airportCount; ++i) {
        const Rec& rec = recs[i];
        auto id = str(rec.idOfs, rec.idLen);
        auto name = str(rec.nameOfs, rec.nameLen);
        if (!id || !name)
            return std::nullopt;
        AirportDef& d = defs[i];
        d.id = std::move(*id);
        d.name = std::move(*name);
        d.latRad = rec.latRad;
        d.lonRad = rec.lonRad;
        d.elevationM = rec.elevationM;
        d.worldX = rec.worldX;
        d.worldZ = rec.worldZ;
        d.acceptsLandings = (rec.flags & 0x01u) != 0;
        d.useWorldXZ = (rec.flags & 0x02u) != 0;
        if (static_cast<std::size_t>(rwCursor) + rec.runwayCount > runwayTotal)
            return std::nullopt;
        d.runways.reserve(rec.runwayCount);
        for (uint16_t k = 0; k < rec.runwayCount; ++k) {
            const RwRec& rr = rws[rwCursor++];
            RunwayDef rw;
            rw.headingDeg = rr.heading;
            rw.lengthM = rr.length;
            rw.widthM = rr.width;
            rw.surface = static_cast<RunwaySurface>(rr.surface > static_cast<uint8_t>(RunwaySurface::Deck)
                                                        ? static_cast<uint8_t>(RunwaySurface::Asphalt)
                                                        : rr.surface);
            d.runways.push_back(rw);
        }
    }
    if (rwCursor != runwayTotal)
        return std::nullopt;

    return defs;
}

} // namespace fl
