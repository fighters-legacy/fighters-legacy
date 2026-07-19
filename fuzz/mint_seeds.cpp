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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <net/BitStream.h>
#include <net/GameProtocol.h>
#include <net/SnapshotCodec.h>
#include <net/WireCodec.h>

#include <RconServer.h>
#include <server_config.h>

#include "ogg_fixture.h"

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

void writeSeed(const std::string& harness, const std::string& name, std::string_view text) {
    writeSeed(harness, name, std::vector<uint8_t>(text.begin(), text.end()));
}

// Serialize a wire struct to bytes (via the same appendMsg the server/client use).
template <typename T> std::vector<uint8_t> wireBytes(const T& msg) {
    std::vector<uint8_t> b;
    fl::appendMsg(b, msg);
    return b;
}

// Append a length-prefixed frame ([len: uint16_t LE][bytes]) — the format fuzz/FuzzFrames.h reads,
// so a single seed can carry several onReceive() packets in order for the handler harnesses.
void appendFrame(std::vector<uint8_t>& out, const std::vector<uint8_t>& msg) {
    const uint16_t len = static_cast<uint16_t>(msg.size());
    out.push_back(static_cast<uint8_t>(len & 0xFFu));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
    out.insert(out.end(), msg.begin(), msg.end());
}

// A null-terminated field-copy into a fixed wire char[] (mirrors how the server fills these).
template <std::size_t N> void setField(char (&dst)[N], const char* src) {
    std::strncpy(dst, src, N - 1);
}

// Mirror of tests/test_client_net_event_handler.cpp buildSnapshotPkt: header + shared-origin table
// (#725: one origin at index 0) + byte-aligned stitched record stream. An optional TLV block is
// appended after the stream.
std::vector<uint8_t> buildSnapshotPkt(uint64_t tick, const std::vector<fl::QuantEntity>& recs, const double origin[3],
                                      const std::vector<uint8_t>& tlv = {}) {
    std::vector<uint8_t> stream;
    for (const auto& qe : recs) {
        std::vector<uint8_t> blob;
        fl::encodeStandaloneRecord(blob, qe, origin, /*sendGen=*/qe.isFull);
        fl::appendStitchedRecord(stream, /*originIndex=*/0u, blob);
    }

    std::vector<uint8_t> buf;
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.tickIndex = tick;
    hdr.recordCount = static_cast<uint16_t>(recs.size());
    hdr.originCount = 1u;
    hdr.bitstreamBytes = static_cast<uint32_t>(stream.size());
    fl::appendMsg(buf, hdr);
    const auto* op = reinterpret_cast<const uint8_t*>(origin);
    buf.insert(buf.end(), op, op + 3u * sizeof(double));
    buf.insert(buf.end(), stream.begin(), stream.end());
    buf.insert(buf.end(), tlv.begin(), tlv.end());
    return buf;
}

// A zstd frame containing the payload as a single RAW block (RFC 8878: magic, single-segment frame
// header with a 1-byte Frame_Content_Size, one last/raw block). Hand-assembled so regeneration is
// byte-stable: linking libzstd here would make the committed corpus depend on the system zstd
// version. Any zstd decoder accepts it, so the #775 client decompress path gets real coverage.
std::vector<uint8_t> rawZstdFrame(const std::vector<uint8_t>& payload) {
    if (payload.size() > 255u) {
        std::fprintf(stderr, "rawZstdFrame: payload %zu exceeds the 1-byte FCS form\n", payload.size());
        std::exit(1);
    }
    std::vector<uint8_t> f{0x28u,
                           0xB5u,
                           0x2Fu,
                           0xFDu, // magic (LE 0xFD2FB528)
                           0x20u, // FHD: single-segment, 1-byte content size
                           static_cast<uint8_t>(payload.size())};
    const auto bh = static_cast<uint32_t>((payload.size() << 3) | 0x1u); // last=1, type=raw
    f.push_back(static_cast<uint8_t>(bh & 0xFFu));
    f.push_back(static_cast<uint8_t>((bh >> 8) & 0xFFu));
    f.push_back(static_cast<uint8_t>((bh >> 16) & 0xFFu));
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
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

// --- Sub 2 (#709) harnesses ---

void mintServerMsgSeeds() {
    printf("fuzz_server_msg:\n");

    // seed-input: a valid MsgClientInput frame followed by a MsgHeartbeat frame.
    std::vector<uint8_t> input;
    fl::MsgClientInput inp{};
    inp.seqNum = 1;
    inp.throttle = 0.5f;
    inp.aileron = 0.25f;
    appendFrame(input, wireBytes(inp));
    fl::MsgHeartbeat hb{};
    hb.tickIndex = 1;
    appendFrame(input, wireBytes(hb));
    writeSeed("fuzz_server_msg", "seed-input.bin", input);

    // seed-admin: a MsgAdminCommand frame carrying the harness's operator token ("fz") + "status".
    std::vector<uint8_t> admin;
    fl::MsgAdminCommand ac{};
    ac.reqId = 7;
    setField(ac.token, "fz");
    setField(ac.command, "status");
    appendFrame(admin, wireBytes(ac));
    writeSeed("fuzz_server_msg", "seed-admin.bin", admin);

    // seed-wingman: a MsgClientInput (so the peer has a viewAxis for boresight designation) followed
    // by a MsgWingmanCommand order (#610). The order path is reachable from the network, so it is
    // fuzzed like every other handler.
    std::vector<uint8_t> wing;
    fl::MsgClientInput wi{};
    wi.seqNum = 1;
    wi.viewAxis[0] = 1.f;
    appendFrame(wing, wireBytes(wi));
    fl::MsgWingmanCommand wc{};
    wc.command = 1; // engage_bandits
    wc.seqNum = 1;
    wc.flightId = fl::kOwnFlight;
    appendFrame(wing, wireBytes(wc));
    writeSeed("fuzz_server_msg", "seed-wingman.bin", wing);

    // seed-seat (#974): a MsgSeatRequest join followed by a leave. The seat-join handler runs untrusted
    // {entityIdx, seatIndex} through evaluateSeatRequest before touching any crew state, so it is fuzzed.
    std::vector<uint8_t> seat;
    fl::MsgSeatRequest sj{};
    sj.seatIndex = 1;
    sj.entityIdx = 3;
    sj.entityGen = 1;
    appendFrame(seat, wireBytes(sj));
    fl::MsgSeatRequest sl{};
    sl.flags = fl::kSeatRequestFlagLeave;
    appendFrame(seat, wireBytes(sl));
    writeSeed("fuzz_server_msg", "seed-seat.bin", seat);
}

void mintClientMsgSeeds() {
    printf("fuzz_client_msg:\n");
    const double origin[3] = {1000.0, 500.0, -2000.0};

    // seed-handshake: Hello + ConnectAck + a full snapshot + Motd + WeatherState + ServerNotice + PeerDelay.
    std::vector<uint8_t> s;
    appendFrame(s, wireBytes(fl::MsgHello{}));
    fl::MsgConnectAck ack{};
    ack.assignedEntityIdx = 7;
    ack.assignedEntityGen = 3;
    ack.planetRadiusKm = 6371.f;
    appendFrame(s, wireBytes(ack));
    appendFrame(s, buildSnapshotPkt(1, {makeFullRecord()}, origin));
    std::vector<uint8_t> motd = wireBytes(fl::MsgMotdHeader{});
    const char* motdText = "Welcome";
    motd.insert(motd.end(), motdText, motdText + std::strlen(motdText));
    motd.push_back(0u);
    appendFrame(s, motd);
    appendFrame(s, wireBytes(fl::MsgWeatherState{}));
    appendFrame(s, wireBytes(fl::MsgServerNotice{}));
    fl::MsgPeerDelay pd{};
    pd.delayTicks = 6;
    appendFrame(s, wireBytes(pd));
    // The flight check-in + an order ack (#610): the client parses these into the radio menu.
    fl::MsgWingmanAck wa{};
    wa.result = static_cast<uint8_t>(fl::WingmanResult::CheckIn);
    wa.flightSize = 1;
    wa.flightId = 1;
    appendFrame(s, wireBytes(wa));
    writeSeed("fuzz_client_msg", "seed-handshake.bin", s);

    // seed-chunks: a two-part AdminResponseChunk stream (reassembly) + a fast-path AdminResponse + ConnectRefusal.
    std::vector<uint8_t> c;
    fl::MsgAdminResponseChunk c0{};
    c0.reqId = 5;
    c0.seqNum = 0;
    setField(c0.body, "part one ");
    appendFrame(c, wireBytes(c0));
    fl::MsgAdminResponseChunk c1{};
    c1.reqId = 5;
    c1.seqNum = 1;
    c1.flags = fl::kChunkFlagEnd;
    setField(c1.body, "part two");
    appendFrame(c, wireBytes(c1));
    fl::MsgAdminResponse ar{};
    ar.reqId = 9;
    setField(ar.text, "ok");
    appendFrame(c, wireBytes(ar));
    fl::MsgConnectRefusal cr{};
    cr.code = 1;
    setField(cr.reason, "banned");
    appendFrame(c, wireBytes(cr));
    writeSeed("fuzz_client_msg", "seed-chunks.bin", c);

    // seed-compressed: a snapshot in the #775 compressed wire form (flags bit 0 + uncompressedBytes
    // in the header, zstd frame after it). Multiple records so mutations around record boundaries
    // stay interesting after decompression.
    std::vector<uint8_t> zc;
    std::vector<fl::QuantEntity> recs;
    for (uint32_t i = 0; i < 5u; ++i) {
        fl::QuantEntity e = makeFullRecord();
        e.idx = 7u + i;
        e.pos[0] += 40.0 * i;
        recs.push_back(e);
    }
    std::vector<uint8_t> rawPkt = buildSnapshotPkt(2, recs, origin);
    const std::size_t hdrSize = sizeof(fl::MsgWorldSnapshotHeader);
    std::vector<uint8_t> payload(rawPkt.begin() + static_cast<std::ptrdiff_t>(hdrSize), rawPkt.end());
    fl::MsgWorldSnapshotHeader zhdr{};
    std::memcpy(&zhdr, rawPkt.data(), hdrSize);
    zhdr.flags |= fl::kSnapshotFlagCompressed;
    zhdr.uncompressedBytes = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> zpkt(rawPkt.begin(), rawPkt.begin() + static_cast<std::ptrdiff_t>(hdrSize));
    std::memcpy(zpkt.data(), &zhdr, hdrSize);
    const std::vector<uint8_t> frame = rawZstdFrame(payload);
    zpkt.insert(zpkt.end(), frame.begin(), frame.end());
    appendFrame(zc, zpkt);
    writeSeed("fuzz_client_msg", "seed-compressed.bin", zc);

    // seed-crew (#972): a MsgCrewRoster (header + two seat records) followed by a snapshot carrying a
    // SnapshotCrew TLV (one crewed entity, one turret). Exercises the roster reader and the crew-TLV
    // decoder so mutations around the seat-count / turret-count fields stay interesting.
    std::vector<uint8_t> cw;
    std::vector<uint8_t> roster;
    fl::MsgCrewRosterHeader chdr{};
    chdr.seatCount = 2;
    chdr.turretCount = 1;
    chdr.entityIdx = 7;
    chdr.entityGen = 3;
    fl::appendMsg(roster, chdr);
    fl::CrewRosterSeat cs0{};
    cs0.occupancy = static_cast<uint8_t>(fl::SeatOccupancy::Human);
    cs0.capabilities = 0x03;
    setField(cs0.role, "pilot");
    fl::appendMsg(roster, cs0);
    fl::CrewRosterSeat cs1{};
    cs1.seatIndex = 1;
    cs1.occupancy = static_cast<uint8_t>(fl::SeatOccupancy::Bot);
    cs1.capabilities = 0x02;
    cs1.turretIndex = 0;
    setField(cs1.role, "gunner");
    fl::appendMsg(roster, cs1);
    appendFrame(cw, roster);

    std::vector<uint8_t> crewSnapPkt = buildSnapshotPkt(3, {makeFullRecord()}, origin);
    std::vector<uint8_t> crewTlv;
    crewTlv.push_back(1u); // entryCount
    const uint32_t crewIdx = 7;
    crewTlv.insert(crewTlv.end(), reinterpret_cast<const uint8_t*>(&crewIdx),
                   reinterpret_cast<const uint8_t*>(&crewIdx) + 4);
    crewTlv.push_back(1u); // turretCount
    const int16_t azQ = 16000, elQ = -8000;
    crewTlv.insert(crewTlv.end(), reinterpret_cast<const uint8_t*>(&azQ), reinterpret_cast<const uint8_t*>(&azQ) + 2);
    crewTlv.insert(crewTlv.end(), reinterpret_cast<const uint8_t*>(&elQ), reinterpret_cast<const uint8_t*>(&elQ) + 2);
    fl::appendExtRaw(crewSnapPkt, static_cast<uint16_t>(fl::ExtTag::SnapshotCrew), crewTlv.data(),
                     static_cast<uint16_t>(crewTlv.size()));
    appendFrame(cw, crewSnapPkt);
    writeSeed("fuzz_client_msg", "seed-crew.bin", cw);
}

void mintAssetValidatorSeeds() {
    printf("fuzz_asset_validator:\n");
    // Layout: [type-selector byte][magic bytes...]; the harness does type = byte0 % AssetType::Count.
    auto seed = [](uint8_t typeSel, std::vector<uint8_t> magic) {
        std::vector<uint8_t> b;
        b.push_back(typeSel);
        b.insert(b.end(), magic.begin(), magic.end());
        return b;
    };
    writeSeed("fuzz_asset_validator", "seed-mesh-glb.bin", seed(0, {0x67, 0x6C, 0x54, 0x46, 0x02, 0, 0, 0})); // "glTF"
    writeSeed("fuzz_asset_validator", "seed-texture-png.bin",
              seed(1, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}));                       // PNG
    writeSeed("fuzz_asset_validator", "seed-audio-ogg.bin", seed(2, {0x4F, 0x67, 0x67, 0x53})); // "OggS"
}

void mintTerrainPngSeeds() {
    printf("fuzz_terrain_png:\n");
    // A tiny valid 4x4 16-bit grayscale PNG. Generated once with Python zlib+struct (color type 0,
    // bit depth 16); the exact byte-writer is recorded in fuzz/fuzz_terrain_png notes. Kept inline so
    // the seed is regenerated byte-stably without a checked-in .png tool dependency.
    static const uint8_t png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00,
        0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x0a, 0x1d, 0xe1, 0x00,
        0x00, 0x00, 0x2c, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x60, 0x60, 0xd0, 0x60, 0x08, 0x60,
        0xa8, 0x60, 0x60, 0x48, 0x61, 0xe8, 0x61, 0xd8, 0xc2, 0x70, 0x87, 0x81, 0xe1, 0x04, 0xc3, 0x07, 0x46,
        0x09, 0x46, 0x07, 0x06, 0x46, 0x1d, 0xc6, 0x10, 0xc6, 0x1a, 0xc6, 0x25, 0x00, 0x73, 0x70, 0x07, 0x27,
        0x51, 0xb9, 0x54, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    writeSeed("fuzz_terrain_png", "seed-4x4-gray16.bin", std::vector<uint8_t>(std::begin(png), std::end(png)));
}

void mintOggSeeds() {
    printf("fuzz_ogg:\n");
    // Real minimal OGG Vorbis stream (0.05 s mono 44.1 kHz silence), shared with
    // tests/test_music_manager.cpp via tests/ogg_fixture.h so the two never drift.
    writeSeed("fuzz_ogg", "seed-silence.bin",
              std::vector<uint8_t>(std::begin(fl::kMinimalOgg), std::end(fl::kMinimalOgg)));
    // Truncated first page — a structurally interesting invalid stream so the mutator
    // starts on the open-failure/reject paths as well as the happy path.
    writeSeed("fuzz_ogg", "seed-truncated.bin",
              std::vector<uint8_t>(std::begin(fl::kMinimalOgg), std::begin(fl::kMinimalOgg) + 64));
}

void mintTomlSeeds() {
    // Minimal valid documents cribbed from the corresponding unit-test fixtures so the seeds start on
    // the parser success path; the fuzzer + dictionaries explore malformed variants from there.
    static constexpr std::string_view kFlightModel = R"(
[aircraft]
name         = "Test Fighter"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000.0
mesh         = "test_mesh"
cockpit      = "test_hud"

[flight_model]
mass_kg      = 10000.0
wing_area_m2 = 35.0
wingspan_m   = 10.0
mac_m        = 3.5
fuel_kg      = 4000.0
ixx_kg_m2    = 10000.0
iyy_kg_m2    = 70000.0
izz_kg_m2    = 78000.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0]
mach   = [0.3, 0.9]
values = [-0.2, -0.2, 0.05, 0.05, 0.4, 0.4, 0.75, 0.75, 1.05, 1.05]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.07
gear_cd       = 0.03

[aero.moments]
cm_alpha = -0.7
cm_q     = -10.0
cm_de    = -1.0
cl_beta  = -0.08
cl_p     = -0.40
cl_da    =  0.07
cn_beta  =  0.10
cn_r     = -0.12
cn_dr    = -0.05
)";
    static constexpr std::string_view kEntityDef = R"(
[entity]
id       = "test:fighter"
name     = "Test Fighter"
category = "air_vehicle"
max_hp   = 100.0
mesh     = "aircraft/test"
)";
    static constexpr std::string_view kPlaylist = R"(
[crossfade]
duration_s = 2.5

[[states]]
id = "Menu"
tracks = ["music/menu"]
loop = true
)";

    printf("fuzz_flight_model_toml:\n");
    writeSeed("fuzz_flight_model_toml", "seed-minimal.bin", kFlightModel);
    printf("fuzz_entity_def_toml:\n");
    writeSeed("fuzz_entity_def_toml", "seed-minimal.bin", kEntityDef);
    printf("fuzz_playlist_toml:\n");
    writeSeed("fuzz_playlist_toml", "seed-minimal.bin", kPlaylist);
    printf("fuzz_server_config_toml:\n");
    writeSeed("fuzz_server_config_toml", "seed-default.bin", fl::defaultServerConfigToml());
}

void mintModManifestSeeds() {
    printf("fuzz_mod_manifest:\n");
    const std::string manifest = "[mod]\nname = \"Test Mod\"\nid = \"test-mod\"\nversion = \"1.0.0\"\n"
                                 "\"engine-api\" = \"1.0\"\npriority = 10\ndepends = []\n";
    writeSeed("fuzz_mod_manifest", "seed-valid.bin", std::string_view("test-mod\n" + manifest));
    // A path-traversal pack name exercising the directory-name sanitizer.
    writeSeed("fuzz_mod_manifest", "seed-traversal.bin", std::string_view("../../etc\n" + manifest));
}

void mintMeshJsonSeeds() {
    printf("fuzz_mesh_json:\n");
    static constexpr std::string_view kGltf = R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "fa18c", "mesh": 0}],
  "meshes": [{"name": "fa18c", "primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{
    "bufferView": 0, "componentType": 5126, "count": 3,
    "type": "VEC3", "max": [1,1,1], "min": [0,0,0]
  }],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
})json";
    writeSeed("fuzz_mesh_json", "seed-minimal.bin", kGltf);
}

} // namespace

int main() {
    printf("Minting fuzz seed corpora into %s\n", FL_FUZZ_CORPUS_DIR);
    mintSnapshotSeeds();
    mintWireTlvSeeds();
    mintRconSeeds();
    mintServerMsgSeeds();
    mintClientMsgSeeds();
    mintAssetValidatorSeeds();
    mintTerrainPngSeeds();
    mintOggSeeds();
    mintTomlSeeds();
    mintModManifestSeeds();
    mintMeshJsonSeeds();
    printf("Done.\n");
    return 0;
}
