// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportIndexFile.h"
#include "math/Fnv.h"

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

// ── little-endian writers ─────────────────────────────────────────────────────
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putF32(std::vector<uint8_t>& b, float v) {
    putU32(b, std::bit_cast<uint32_t>(v));
}
void putF64(std::vector<uint8_t>& b, double v) {
    putU64(b, std::bit_cast<uint64_t>(v));
}

// ── little-endian readers (bounds-checked via a cursor) ───────────────────────
struct Reader {
    std::span<const uint8_t> b;
    std::size_t pos{0};
    bool ok{true};

    uint16_t u16() {
        if (pos + 2 > b.size()) {
            ok = false;
            return 0;
        }
        const uint16_t v = static_cast<uint16_t>(b[pos]) | static_cast<uint16_t>(b[pos + 1] << 8);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (pos + 4 > b.size()) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(b[pos + static_cast<std::size_t>(i)]) << (8 * i);
        pos += 4;
        return v;
    }
    uint64_t u64() {
        if (pos + 8 > b.size()) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(b[pos + static_cast<std::size_t>(i)]) << (8 * i);
        pos += 8;
        return v;
    }
    float f32() {
        return std::bit_cast<float>(u32());
    }
    double f64() {
        return std::bit_cast<double>(u64());
    }
    uint8_t u8() {
        if (pos + 1 > b.size()) {
            ok = false;
            return 0;
        }
        return b[pos++];
    }
};

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

    putU32(out, kMagic);
    putU32(out, kVersion);
    putU64(out, sourceHash);
    putU32(out, static_cast<uint32_t>(defs.size()));
    putU32(out, runwayTotal);
    putU32(out, static_cast<uint32_t>(blob.size()));

    for (std::size_t i = 0; i < defs.size(); ++i) {
        const AirportDef& d = defs[i];
        putU32(out, idRefs[i].ofs);
        putU16(out, idRefs[i].len);
        putU32(out, nameRefs[i].ofs);
        putU16(out, nameRefs[i].len);
        putF64(out, d.latRad);
        putF64(out, d.lonRad);
        putF64(out, d.elevationM);
        uint8_t flags = 0;
        if (d.acceptsLandings)
            flags |= 0x01u;
        if (d.useWorldXZ)
            flags |= 0x02u;
        out.push_back(flags);
        out.push_back(0); // pad
        putF64(out, d.worldX);
        putF64(out, d.worldZ);
        putU16(out, static_cast<uint16_t>(d.runways.size()));
        putU16(out, 0); // pad
    }

    for (const auto& d : defs) {
        for (const RunwayDef& rw : d.runways) {
            putF32(out, rw.headingDeg);
            putF32(out, rw.lengthM);
            putF32(out, rw.widthM);
            out.push_back(static_cast<uint8_t>(rw.surface));
        }
    }

    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

std::optional<std::vector<AirportDef>> readAirportIndex(std::span<const uint8_t> bytes, uint64_t expectedSourceHash) {
    Reader r{bytes, 0, true};
    if (r.u32() != kMagic || r.u32() != kVersion)
        return std::nullopt;
    if (r.u64() != expectedSourceHash)
        return std::nullopt;
    const uint32_t airportCount = r.u32();
    const uint32_t runwayTotal = r.u32();
    const uint32_t blobSize = r.u32();
    if (!r.ok)
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
    if (!r.ok)
        return std::nullopt;

    const std::size_t blobStart = r.pos;
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
