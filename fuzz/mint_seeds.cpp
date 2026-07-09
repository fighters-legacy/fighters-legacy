// SPDX-License-Identifier: GPL-3.0-or-later

// Seed-corpus mint tool for the fuzz harnesses (#708). NOT a fuzzer — it has its own main and
// regenerates the tiny synthetic seed-*.bin files under fuzz/corpus/<name>/ from the SAME builders
// the unit tests use (buildSnapshotPkt, appendExt/appendExtRaw, encodePacket). Seeds are valid
// wire artifacts so libFuzzer starts from real coverage; they are never copyrighted assets.
//
// Run once after building the fuzz preset:  ./build/fuzz/fuzz/fuzz-mint-seeds
// Regenerating must be byte-stable (git diff clean) so the committed corpus stays reproducible.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <net/BitStream.h>
#include <net/GameProtocol.h>
#include <net/SnapshotCodec.h>
#include <net/WireCodec.h>

#include <RconServer.h>

#ifndef FL_FUZZ_CORPUS_DIR
#error "FL_FUZZ_CORPUS_DIR must be defined by CMake"
#endif

namespace fs = std::filesystem;

namespace {

void writeSeed(const std::string& harness, const std::string& name, const std::vector<uint8_t>& bytes) {
    const fs::path dir = fs::path(FL_FUZZ_CORPUS_DIR) / harness;
    fs::create_directories(dir);
    const fs::path out = dir / name;
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    printf("  wrote %s (%zu bytes)\n", out.string().c_str(), bytes.size());
}

// Mirror of tests/test_client_net_event_handler.cpp buildSnapshotPkt: header + byte-aligned
// quantized record bitstream, header back-patched with recordCount/bitstreamBytes. An optional TLV
// block is appended after the bitstream.
std::vector<uint8_t> buildSnapshotPkt(uint64_t tick, const std::vector<fl::QuantEntity>& recs, const double origin[3],
                                      const std::vector<uint8_t>& tlv = {}) {
    fl::BitWriter w;
    uint32_t prev = 0;
    for (const auto& qe : recs)
        fl::encodeRecord(w, qe, prev, origin, /*sendGen=*/qe.isFull);
    w.alignToByte();

    std::vector<uint8_t> buf;
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.tickIndex = tick;
    hdr.frameOrigin[0] = origin[0];
    hdr.frameOrigin[1] = origin[1];
    hdr.frameOrigin[2] = origin[2];
    fl::appendMsg(buf, hdr);
    buf.insert(buf.end(), w.bytes().begin(), w.bytes().end());
    hdr.recordCount = static_cast<uint16_t>(recs.size());
    hdr.bitstreamBytes = static_cast<uint32_t>(w.byteCount());
    fl::writeMsgAt(buf, 0, hdr);
    buf.insert(buf.end(), tlv.begin(), tlv.end());
    return buf;
}

fl::QuantEntity makeFullRecord() {
    fl::QuantEntity e;
    e.idx = 7;
    e.gen = 3;
    e.typeIndex = 42;
    e.isFull = true;
    e.hasOmega = true;
    e.pos[0] = 12.5;
    e.pos[1] = -3.0;
    e.pos[2] = 250.0;
    e.vel[0] = 120.f;
    e.vel[1] = -8.f;
    e.vel[2] = 33.f;
    e.quat[2] = 0.7071f;
    e.quat[3] = 0.7071f;
    e.omega[0] = 0.5f;
    e.omega[1] = -1.2f;
    e.omega[2] = 0.05f;
    e.damageLevel = 2;
    e.engineFailFlags = 0x10;
    e.throttle = 88;
    e.fuelPct = 47;
    e.abEngaged = true;
    e.playerOwned = true;
    return e;
}

void mintSnapshotSeeds() {
    printf("fuzz_snapshot_codec:\n");
    const double origin[3] = {1000.0, 500.0, -2000.0};

    // seed-full: one full own-entity record (every field populated).
    writeSeed("fuzz_snapshot_codec", "seed-full.bin", buildSnapshotPkt(1, {makeFullRecord()}, origin));

    // seed-full-delta: a full record followed by a delta record (omits type/gen/omega).
    fl::QuantEntity delta;
    delta.idx = 9;
    delta.isFull = false;
    delta.pos[0] = 4.0;
    delta.throttle = 50;
    writeSeed("fuzz_snapshot_codec", "seed-full-delta.bin", buildSnapshotPkt(2, {makeFullRecord(), delta}, origin));

    // seed-tlv: a full record plus the trailing TLV block the client scans (peer count + despawn list).
    std::vector<uint8_t> tlv;
    fl::appendExt<uint16_t>(tlv, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), 4);
    const uint32_t despawn[] = {11u, 12u};
    fl::appendExtRaw(tlv, static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn), despawn, sizeof(despawn));
    writeSeed("fuzz_snapshot_codec", "seed-tlv.bin", buildSnapshotPkt(3, {makeFullRecord()}, origin, tlv));
}

void mintWireTlvSeeds() {
    printf("fuzz_wire_tlv:\n");

    // seed-fixed: two fixed-width uint16 extensions back to back.
    std::vector<uint8_t> fixed;
    fl::appendExt<uint16_t>(fixed, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), 8);
    fl::appendExt<uint16_t>(fixed, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), 25);
    writeSeed("fuzz_wire_tlv", "seed-fixed.bin", fixed);

    // seed-despawn: a variable-length uint32_t[] despawn payload (the raw-TLV path).
    std::vector<uint8_t> var;
    const uint32_t ids[] = {1u, 2u, 3u};
    fl::appendExtRaw(var, static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn), ids, sizeof(ids));
    writeSeed("fuzz_wire_tlv", "seed-despawn.bin", var);
}

void mintRconSeeds() {
    printf("fuzz_rcon_packet:\n");

    // seed-auth: an AUTH request packet.
    writeSeed("fuzz_rcon_packet", "seed-auth.bin", fl::rcon::encodePacket(1, fl::rcon::kTypeAuth, "password"));

    // seed-exec: an EXECCOMMAND packet ("status").
    writeSeed("fuzz_rcon_packet", "seed-exec.bin", fl::rcon::encodePacket(42, fl::rcon::kTypeExecCommand, "status"));

    // seed-two: two packets in one buffer (drives the multi-packet drain loop).
    auto a = fl::rcon::encodePacket(1, fl::rcon::kTypeAuth, "pw");
    auto b = fl::rcon::encodePacket(2, fl::rcon::kTypeExecCommand, "help");
    a.insert(a.end(), b.begin(), b.end());
    writeSeed("fuzz_rcon_packet", "seed-two.bin", a);
}

} // namespace

int main() {
    printf("Minting fuzz seed corpora into %s\n", FL_FUZZ_CORPUS_DIR);
    mintSnapshotSeeds();
    mintWireTlvSeeds();
    mintRconSeeds();
    printf("Done.\n");
    return 0;
}
