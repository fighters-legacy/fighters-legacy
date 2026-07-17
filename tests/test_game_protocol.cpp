// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/SnapshotCodec.h"
#include "net/WireCodec.h"
#include "weather/WeatherTypes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

TEST_CASE("GameProtocol: wire struct sizes match natural-aligned layout", "[game_protocol]") {
    CHECK(sizeof(fl::MsgHello) == 4u);
    CHECK(sizeof(fl::MsgConnectAck) == 20u);      // #853: +grantedRole @16 (still record-aligned)
    CHECK(sizeof(fl::MsgConnectRequest) == 72u);  // #853: role + entity type + trailing pack manifest
    CHECK(sizeof(fl::PackManifestEntry) == 128u); // #872 wire half: id + version + reserved hash
    CHECK(sizeof(fl::MsgEntityTypeDef) == 336u);  // #886 tail-appended category/projectileKind (332 -> 336)
    CHECK(sizeof(fl::MsgFactionDef) == 132u);     // #860: 1+1+2 + 64 + 64
    CHECK(offsetof(fl::MsgEntityTypeDef, name) == 268u);
    CHECK(offsetof(fl::MsgEntityTypeDef, category) == 332u);
    CHECK(offsetof(fl::MsgEntityTypeDef, projectileKind) == 333u);
    CHECK(offsetof(fl::MsgFactionDef, factionIndex) == 2u);
    CHECK(offsetof(fl::MsgFactionDef, id) == 4u);
    CHECK(offsetof(fl::MsgFactionDef, name) == 68u);
    CHECK(sizeof(fl::MsgWorldSnapshotHeader) == 24u); // #725: origin table + record stream follow the header
    CHECK(sizeof(fl::MsgClientInput) == 80u);         // #858: +cameraEye[3] double @56 (observer interest)
    CHECK(sizeof(fl::MsgHeartbeat) == 16u);
    CHECK(sizeof(fl::MsgAdminCommand) == 128u);
    CHECK(sizeof(fl::MsgAdminResponse) == 128u);
    CHECK(sizeof(fl::MsgAdminResponseChunk) == 512u);
    CHECK(sizeof(fl::MsgMotdHeader) == 4u);
    CHECK(sizeof(fl::MsgConnectRefusal) == 64u);
}

TEST_CASE("GameProtocol: MsgId space reserves 0x00-0x1F for ENet, 0x20+ for raw UDP (#853)", "[game_protocol]") {
    // The non-ENet boundary was raised from 0x10 to 0x20 to free an ENet id for ConnectRequest.
    CHECK(static_cast<uint8_t>(fl::MsgId::ConnectRequest) == 0x11u);
    CHECK(static_cast<uint8_t>(fl::MsgId::LanBeacon) == 0x20u);
    CHECK(static_cast<uint8_t>(fl::MsgId::CombatEvent) < 0x20u);
}

TEST_CASE("GameProtocol: MsgConnectRequest field offsets + role validation (#853)", "[game_protocol]") {
    CHECK(offsetof(fl::MsgConnectRequest, protocolVersion) == 2u);
    CHECK(offsetof(fl::MsgConnectRequest, packCount) == 4u);
    CHECK(offsetof(fl::MsgConnectRequest, requestedEntityType) == 8u);
    CHECK(offsetof(fl::MsgConnectAck, grantedRole) == 16u);
    CHECK(offsetof(fl::PackManifestEntry, version) == 64u);
    CHECK(offsetof(fl::PackManifestEntry, contentHash) == 96u);

    CHECK(fl::isPeerRoleOrdinal(static_cast<uint8_t>(fl::PeerRole::Pilot)));
    CHECK(fl::isPeerRoleOrdinal(static_cast<uint8_t>(fl::PeerRole::Observer)));
    CHECK_FALSE(fl::isPeerRoleOrdinal(2u)); // out-of-grammar byte from an untrusted client
    CHECK_FALSE(fl::isPeerRoleOrdinal(255u));
}

TEST_CASE("GameProtocol: MsgConnectRequest round-trips role, entity type, and trailing manifest (#853/#872)",
          "[game_protocol]") {
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    req.packCount = 2;
    std::snprintf(req.requestedEntityType, sizeof(req.requestedEntityType), "fl-base:f5e");

    fl::PackManifestEntry a{};
    std::snprintf(a.id, sizeof(a.id), "fl-base");
    std::snprintf(a.version, sizeof(a.version), "0.3.1");
    fl::PackManifestEntry b{};
    std::snprintf(b.id, sizeof(b.id), "theater-pack");
    std::snprintf(b.version, sizeof(b.version), "1.0.0");

    std::vector<uint8_t> buf;
    fl::appendMsg(buf, req);
    fl::appendMsg(buf, a);
    fl::appendMsg(buf, b);
    REQUIRE(buf.size() == sizeof(req) + 2u * sizeof(fl::PackManifestEntry));

    fl::MsgConnectRequest outReq{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), outReq));
    CHECK(outReq.requestedRole == static_cast<uint8_t>(fl::PeerRole::Observer));
    CHECK(outReq.packCount == 2u);
    CHECK(std::string(outReq.requestedEntityType) == "fl-base:f5e");

    std::size_t off = sizeof(req);
    fl::PackManifestEntry outA{};
    REQUIRE(fl::readRecordAt(buf.data(), buf.size(), off, outA));
    CHECK(std::string(outA.id) == "fl-base");
    CHECK(std::string(outA.version) == "0.3.1");
    off += sizeof(fl::PackManifestEntry);
    fl::PackManifestEntry outB{};
    REQUIRE(fl::readRecordAt(buf.data(), buf.size(), off, outB));
    CHECK(std::string(outB.id) == "theater-pack");
}

TEST_CASE("GameProtocol: MsgEntityTypeDef field offsets (#811 tail-append is additive)", "[game_protocol]") {
    // The three new fields were appended at the tail precisely so every offset above them is
    // unchanged -- that is what makes this an additive change and keeps kProtocolVersion at 1.
    CHECK(offsetof(fl::MsgEntityTypeDef, id) == 4u);
    CHECK(offsetof(fl::MsgEntityTypeDef, mesh) == 68u);
    CHECK(offsetof(fl::MsgEntityTypeDef, dmgMesh) == 132u);
    CHECK(offsetof(fl::MsgEntityTypeDef, flightModel) == 196u); // starts where the struct used to end
    CHECK(offsetof(fl::MsgEntityTypeDef, payloadMassKg) == 260u);
    CHECK(offsetof(fl::MsgEntityTypeDef, payloadCd0) == 264u);
}

TEST_CASE("GameProtocol: MsgEntityTypeDef round-trips flightModel and payload", "[game_protocol]") {
    fl::MsgEntityTypeDef td{};
    td.typeIndex = 7;
    std::snprintf(td.id, sizeof(td.id), "fl-base:f5e");
    std::snprintf(td.flightModel, sizeof(td.flightModel), "f5e");
    td.payloadMassKg = 412.5f;
    td.payloadCd0 = 0.0031f;
    td.category = 3;       // ObjectCategory::Projectile ordinal (#886)
    td.projectileKind = 2; // ProjectileKind::Bomb ordinal (#886)

    std::vector<uint8_t> buf;
    fl::appendMsg(buf, td);
    REQUIRE(buf.size() == sizeof(fl::MsgEntityTypeDef));

    fl::MsgEntityTypeDef out{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), out));
    CHECK(std::string(out.flightModel) == "f5e");
    CHECK(out.payloadMassKg == Catch::Approx(412.5f));
    CHECK(out.payloadCd0 == Catch::Approx(0.0031f));
    CHECK(out.category == 3u);
    CHECK(out.projectileKind == 2u);
}

TEST_CASE("GameProtocol: a fully-populated flightModel field is NUL-terminated by the reader", "[game_protocol]") {
    // A hostile server can fill the array with no terminator; the client force-terminates before
    // treating it as a C string. Assert the truncation is safe and lossy in the expected way.
    fl::MsgEntityTypeDef td{};
    std::memset(td.flightModel, 'a', sizeof(td.flightModel)); // 64 bytes, no NUL

    std::vector<uint8_t> buf;
    fl::appendMsg(buf, td);
    fl::MsgEntityTypeDef out{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), out));

    out.flightModel[sizeof(out.flightModel) - 1] = '\0'; // what ClientNetEventHandler does
    CHECK(std::strlen(out.flightModel) == sizeof(out.flightModel) - 1);
}

TEST_CASE("GameProtocol: wire structs are naturally aligned for zero-copy", "[game_protocol]") {
    // The origin table that follows the header is double[3] entries, so the header must be 8-aligned
    // and a multiple of 8 — the byte-aligned record stream after it is byte-addressed (#725).
    CHECK(alignof(fl::MsgWorldSnapshotHeader) == 8u);
    CHECK(sizeof(fl::MsgWorldSnapshotHeader) % 8u == 0u);
    CHECK(alignof(fl::MsgClientInput) == 8u);
    // #858: the camera-eye double[3] must land 8-aligned so the naturally-aligned parse stays valid.
    CHECK(offsetof(fl::MsgClientInput, cameraEye) == 56u);
    CHECK(offsetof(fl::MsgClientInput, cameraEye) % 8u == 0u);
}

TEST_CASE("GameProtocol: MsgWorldSnapshotHeader field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, recordCount) == 2u);
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, bitstreamBytes) == 4u);
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, tickIndex) == 8u);
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, originCount) == 16u);
}

TEST_CASE("GameProtocol: selective-ack fields (#566)", "[game_protocol]") {
    // ackMask reuses the former trailing/reserved padding — no size change, must stay 4-aligned.
    CHECK(offsetof(fl::MsgClientInput, ackMask) == 44u);
    CHECK(offsetof(fl::MsgHeartbeat, ackMask) == 4u);
    CHECK(offsetof(fl::MsgHeartbeat, tickIndex) == 8u);
}

TEST_CASE("GameProtocol: stays at protocol version 1 in primary development", "[game_protocol]") {
    CHECK(fl::kProtocolVersion == 1u);
}

TEST_CASE("GameProtocol: MsgConnectRefusal field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgConnectRefusal, code) == 1u);
    CHECK(offsetof(fl::MsgConnectRefusal, reason) == 2u);
}

TEST_CASE("GameProtocol: MsgAdminCommand field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgAdminCommand, reqId) == 2u);
    CHECK(offsetof(fl::MsgAdminCommand, token) == 4u);
    CHECK(offsetof(fl::MsgAdminCommand, command) == 34u);
}

TEST_CASE("GameProtocol: MsgAdminResponse field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgAdminResponse, reqId) == 2u);
    CHECK(offsetof(fl::MsgAdminResponse, text) == 4u);
}

TEST_CASE("GameProtocol: MsgAdminResponseChunk field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgAdminResponseChunk, flags) == 1u);
    CHECK(offsetof(fl::MsgAdminResponseChunk, reqId) == 2u);
    CHECK(offsetof(fl::MsgAdminResponseChunk, seqNum) == 4u);
    CHECK(offsetof(fl::MsgAdminResponseChunk, body) == 6u);
}

TEST_CASE("GameProtocol: MsgWorldSnapshotHeader round-trip", "[game_protocol]") {
    // The header is the only fixed struct in a snapshot; the origin table + quantized record stream
    // are exercised by test_snapshot_codec. Here we round-trip just the header.
    fl::MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.recordCount = 5u;
    hdr.bitstreamBytes = 123u;
    hdr.tickIndex = 42u;
    hdr.originCount = 3u;

    std::vector<uint8_t> buf(sizeof(hdr));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));

    fl::MsgWorldSnapshotHeader parsed{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), parsed));
    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(parsed.recordCount == 5u);
    CHECK(parsed.bitstreamBytes == 123u);
    CHECK(parsed.tickIndex == 42u);
    CHECK(parsed.originCount == 3u);
}

TEST_CASE("GameProtocol: ExtTag SnapshotPeerDelayTicks TLV encode and decode", "[game_protocol]") {
    std::vector<uint8_t> buf;
    const uint16_t kDelay = 7u;
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerDelayTicks), kDelay);

    uint16_t out{};
    REQUIRE(fl::readExtValue(buf.data(), buf.size(), static_cast<uint16_t>(fl::ExtTag::SnapshotPeerDelayTicks), out));
    CHECK(out == kDelay);

    // Unknown tag must be skipped gracefully (no match, returns false).
    uint16_t notFound{};
    CHECK(!fl::readExtValue(buf.data(), buf.size(), uint16_t{0x9999}, notFound));
}

TEST_CASE("GameProtocol: MsgConnectAck round-trip with two type defs", "[game_protocol]") {
    fl::MsgConnectAck ack;
    ack.msgId = static_cast<uint8_t>(fl::MsgId::ConnectAck);
    ack.tickRateHz = 60;
    ack.typeCount = 2;
    ack.assignedEntityIdx = 7u;
    ack.assignedEntityGen = 3u;

    fl::MsgEntityTypeDef defs[2]{};
    std::snprintf(defs[0].id, sizeof(defs[0].id), "%s", "builtin:debug-entity");
    std::snprintf(defs[1].id, sizeof(defs[1].id), "%s", "builtin:other");

    const std::size_t totalSize = sizeof(ack) + 2 * sizeof(fl::MsgEntityTypeDef);
    std::vector<uint8_t> buf(totalSize);
    std::memcpy(buf.data(), &ack, sizeof(ack));
    std::memcpy(buf.data() + sizeof(ack), defs, 2 * sizeof(fl::MsgEntityTypeDef));

    fl::MsgConnectAck parsedAck;
    std::memcpy(&parsedAck, buf.data(), sizeof(parsedAck));
    CHECK(parsedAck.tickRateHz == 60);
    CHECK(parsedAck.typeCount == 2);
    CHECK(parsedAck.assignedEntityIdx == 7u);
    CHECK(parsedAck.assignedEntityGen == 3u);

    fl::MsgEntityTypeDef td0, td1;
    std::memcpy(&td0, buf.data() + sizeof(ack), sizeof(td0));
    std::memcpy(&td1, buf.data() + sizeof(ack) + sizeof(td0), sizeof(td1));
    CHECK(std::string_view(td0.id) == "builtin:debug-entity");
    CHECK(std::string_view(td1.id) == "builtin:other");
}

TEST_CASE("GameProtocol: MsgClientInput round-trip", "[game_protocol]") {
    fl::MsgClientInput src{};
    src.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    src.buttons = 0x03u; // weaponTrigger + afterburner
    src.seqNum = 12345u;
    src.tickIndex = 9999u;
    src.throttle = 0.75f;
    src.elevator = -0.5f;
    src.aileron = 0.25f;
    src.rudder = -0.1f;
    src.viewAxis[0] = 1.f;
    src.viewAxis[1] = 0.f;
    src.viewAxis[2] = 0.f;

    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgClientInput parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));

    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::ClientInput));
    CHECK(parsed.buttons == 0x03u);
    CHECK(parsed.protocolVersion == fl::kProtocolVersion);
    CHECK(parsed.seqNum == 12345u);
    CHECK(parsed.tickIndex == 9999u);
    CHECK(parsed.throttle == 0.75f);
    CHECK(parsed.elevator == -0.5f);
    CHECK(parsed.aileron == 0.25f);
    CHECK(parsed.rudder == -0.1f);
    CHECK(parsed.viewAxis[0] == 1.f);
    CHECK(parsed.viewAxis[1] == 0.f);
    CHECK(parsed.viewAxis[2] == 0.f);
}

TEST_CASE("GameProtocol: MsgHello round-trip", "[game_protocol]") {
    fl::MsgHello src{};

    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgHello parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));

    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
    CHECK(parsed.protocolVersion == fl::kProtocolVersion);
}

TEST_CASE("GameProtocol: MsgWeatherState round-trip preserves all fields", "[game_protocol][weather]") {
    fl::MsgWeatherState src{};
    src.preset = static_cast<uint8_t>(fl::WeatherPreset::Rain);
    src.timeOfDayTenths = 145u; // 14.5 hours
    src.fogDensity = 0.0003f;
    src.fogStartDist = 8000.f;
    src.windX = 5.5f;
    src.windZ = -2.1f;
    src.turbulenceAmp = 3.25f;

    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgWeatherState parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));

    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::WeatherState));
    CHECK(parsed.preset == static_cast<uint8_t>(fl::WeatherPreset::Rain));
    CHECK(parsed.timeOfDayTenths == 145u);
    CHECK(parsed.fogDensity == 0.0003f);
    CHECK(parsed.fogStartDist == 8000.f);
    CHECK(parsed.windX == 5.5f);
    CHECK(parsed.windZ == -2.1f);
    CHECK(parsed.turbulenceAmp == 3.25f);
    CHECK(sizeof(fl::MsgWeatherState) == 24u); // grew 20 -> 24 for turbulenceAmp (#426)
}

TEST_CASE("GameProtocol: MsgWeatherState round-trip preserves Snow preset value", "[game_protocol][weather]") {
    fl::MsgWeatherState src{};
    src.preset = static_cast<uint8_t>(fl::WeatherPreset::Snow);
    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));
    fl::MsgWeatherState parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));
    CHECK(parsed.preset == static_cast<uint8_t>(fl::WeatherPreset::Snow));
    CHECK(parsed.preset == 5u);
}

TEST_CASE("GameProtocol: MsgWeatherState round-trip preserves Blizzard preset value", "[game_protocol][weather]") {
    fl::MsgWeatherState src{};
    src.preset = static_cast<uint8_t>(fl::WeatherPreset::Blizzard);
    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));
    fl::MsgWeatherState parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));
    CHECK(parsed.preset == static_cast<uint8_t>(fl::WeatherPreset::Blizzard));
    CHECK(parsed.preset == 6u);
}

TEST_CASE("GameProtocol: MsgWeatherState timeOfDayTenths decodes to 14.5 hours", "[game_protocol][weather]") {
    fl::MsgWeatherState ws{};
    ws.timeOfDayTenths = 145u;
    float tod = static_cast<float>(ws.timeOfDayTenths) / 10.f;
    CHECK(tod == 14.5f);
}

TEST_CASE("GameProtocol: MsgAdminCommand round-trip", "[game_protocol]") {
    fl::MsgAdminCommand src{};
    src.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    src.reqId = 0x1234u;
    std::snprintf(src.token, sizeof(src.token), "hunter2");
    std::snprintf(src.command, sizeof(src.command), "spawn builtin:debug-entity 0 500 0");

    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgAdminCommand parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));

    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::AdminCommand));
    CHECK(parsed.reqId == 0x1234u);
    CHECK(std::string(parsed.token) == "hunter2");
    CHECK(std::string(parsed.command) == "spawn builtin:debug-entity 0 500 0");
}

TEST_CASE("GameProtocol: MsgAdminResponse round-trip", "[game_protocol]") {
    fl::MsgAdminResponse src{};
    src.msgId = static_cast<uint8_t>(fl::MsgId::AdminResponse);
    src.reqId = 0x5678u;
    std::snprintf(src.text, sizeof(src.text), "spawn queued");

    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgAdminResponse parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));

    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponse));
    CHECK(parsed.reqId == 0x5678u);
    CHECK(std::string(parsed.text) == "spawn queued");
}

TEST_CASE("GameProtocol: MsgAdminResponseChunk round-trip", "[game_protocol]") {
    fl::MsgAdminResponseChunk src{};
    src.reqId = 0xABCDu;
    src.seqNum = 3u;
    src.flags = fl::kChunkFlagEnd;
    std::snprintf(src.body, sizeof(src.body), "chunk body text");

    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgAdminResponseChunk parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));

    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk));
    CHECK(parsed.reqId == 0xABCDu);
    CHECK(parsed.seqNum == 3u);
    CHECK(parsed.flags == fl::kChunkFlagEnd);
    CHECK(std::string(parsed.body) == "chunk body text");
}

namespace {
// Build a snapshot packet: header + a quantized record bitstream of `count` entities + caller TLVs.
// Returns the buffer; sets hdr.recordCount / hdr.bitstreamBytes correctly. The TLV block begins at
// sizeof(MsgWorldSnapshotHeader) + hdr.bitstreamBytes (the new offset contract).
std::vector<uint8_t> buildSnapshot(uint64_t tick, uint16_t count) {
    const double origin[3] = {0.0, 0.0, 0.0}; // one shared origin at table index 0 (#725)
    std::vector<uint8_t> stream;
    for (uint16_t i = 0; i < count; ++i) {
        fl::QuantEntity e;
        e.idx = 10u + i;
        e.isFull = true;
        e.gen = 1u;
        std::vector<uint8_t> blob;
        fl::encodeStandaloneRecord(blob, e, origin, /*sendGen=*/true);
        fl::appendStitchedRecord(stream, /*originIndex=*/0u, blob);
    }

    std::vector<uint8_t> buf;
    const std::size_t hdrOffset = buf.size();
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.tickIndex = tick;
    fl::appendMsg(buf, hdr); // placeholder
    const auto* op = reinterpret_cast<const uint8_t*>(origin);
    buf.insert(buf.end(), op, op + 3u * sizeof(double)); // origin table (one entry)
    buf.insert(buf.end(), stream.begin(), stream.end());
    hdr.recordCount = count;
    hdr.originCount = 1u;
    hdr.bitstreamBytes = static_cast<uint32_t>(stream.size());
    fl::writeMsgAt(buf, hdrOffset, hdr);
    return buf;
}

// Offset of the TLV block: after the header, the origin table, and the stitched record stream (#725).
std::size_t extOffsetOf(const std::vector<uint8_t>& buf) {
    fl::MsgWorldSnapshotHeader rh{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), rh));
    return sizeof(fl::MsgWorldSnapshotHeader) + static_cast<std::size_t>(rh.originCount) * 3u * sizeof(double) +
           rh.bitstreamBytes;
}
} // namespace

TEST_CASE("WireCodec ext: full WorldSnapshot packet with SnapshotPeerCount extension", "[game_protocol]") {
    std::vector<uint8_t> buf = buildSnapshot(/*tick=*/99u, /*count=*/2u);
    const uint16_t kPeers = 7u;
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), kPeers);

    fl::MsgWorldSnapshotHeader rh{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), rh));
    CHECK(rh.tickIndex == 99u);
    CHECK(rh.recordCount == 2u);

    const std::size_t extOffset = extOffsetOf(buf);
    REQUIRE(buf.size() > extOffset);
    uint16_t pc{};
    CHECK(fl::readExtValue(buf.data() + extOffset, buf.size() - extOffset,
                           static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
    CHECK(pc == kPeers);
}

TEST_CASE("WireCodec ext: SnapshotPeerLatency round-trip", "[game_protocol]") {
    std::vector<uint8_t> buf = buildSnapshot(/*tick=*/7u, /*count=*/1u);
    const uint16_t kLatMs = 120u;
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), kLatMs);

    const std::size_t extOffset = extOffsetOf(buf);
    REQUIRE(buf.size() > extOffset);
    uint16_t lat{};
    CHECK(fl::readExtValue(buf.data() + extOffset, buf.size() - extOffset,
                           static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));
    CHECK(lat == kLatMs);
}

TEST_CASE("WireCodec ext: SnapshotPeerCount and SnapshotPeerLatency coexist in same buffer", "[game_protocol]") {
    std::vector<uint8_t> buf = buildSnapshot(/*tick=*/1u, /*count=*/1u);
    const uint16_t kPeers = 5u;
    const uint16_t kLatMs = 83u;
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), kPeers);
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), kLatMs);

    const std::size_t extOffset = extOffsetOf(buf);
    const auto* ext = buf.data() + extOffset;
    const auto extSz = buf.size() - extOffset;

    uint16_t pc{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
    CHECK(pc == kPeers);

    uint16_t lat{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));
    CHECK(lat == kLatMs);
}

TEST_CASE("WireCodec ext: old-receiver compatibility readMsg succeeds on extended packet", "[game_protocol]") {
    // Old-receiver code (readMsg only) still reads the header and ignores the bitstream + extension.
    std::vector<uint8_t> buf = buildSnapshot(/*tick=*/42u, /*count=*/1u);
    const uint16_t kPeers = 3u;
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), kPeers);

    fl::MsgWorldSnapshotHeader rh{};
    CHECK(fl::readMsg(buf.data(), buf.size(), rh));
    CHECK(rh.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(rh.tickIndex == 42u);
    CHECK(rh.recordCount == 1u);
}

TEST_CASE("GameProtocol: MsgHeartbeat and MsgPeerDelay sizes and alignment", "[game_protocol]") {
    CHECK(sizeof(fl::MsgHeartbeat) == 16u);
    CHECK(sizeof(fl::MsgPeerDelay) == 4u);
    CHECK(alignof(fl::MsgHeartbeat) == 8u);
    CHECK(alignof(fl::MsgPeerDelay) == 2u);
}

TEST_CASE("GameProtocol: MsgHeartbeat field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgHeartbeat, tickIndex) == 8u);
}

TEST_CASE("GameProtocol: MsgPeerDelay field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgPeerDelay, delayTicks) == 2u);
}

TEST_CASE("GameProtocol: MsgHeartbeat round-trip", "[game_protocol]") {
    fl::MsgHeartbeat src;
    src.tickIndex = 0xDEADBEEF12345678ULL;
    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));
    fl::MsgHeartbeat dst;
    CHECK(fl::readMsg(buf.data(), buf.size(), dst));
    CHECK(dst.msgId == static_cast<uint8_t>(fl::MsgId::Heartbeat));
    CHECK(dst.tickIndex == 0xDEADBEEF12345678ULL);
}

TEST_CASE("GameProtocol: MsgPeerDelay round-trip", "[game_protocol]") {
    fl::MsgPeerDelay src;
    src.delayTicks = 42u;
    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));
    fl::MsgPeerDelay dst;
    CHECK(fl::readMsg(buf.data(), buf.size(), dst));
    CHECK(dst.msgId == static_cast<uint8_t>(fl::MsgId::PeerDelay));
    CHECK(dst.delayTicks == 42u);
}

// (The compact entity record round-trip lives in test_snapshot_codec.cpp now that the entity body
// is a quantized bitstream rather than fixed MsgEntityEntry/MsgEntityUpdate structs.)

TEST_CASE("GameProtocol: CombatEvent wire layout", "[game_protocol]") {
    CHECK(sizeof(fl::MsgCombatEventHeader) == 4u);
    CHECK(offsetof(fl::MsgCombatEventHeader, count) == 1u);
    CHECK(sizeof(fl::CombatEventRecord) == 32u);
    CHECK(alignof(fl::CombatEventRecord) == 4u);
    CHECK(offsetof(fl::CombatEventRecord, subjectIdx) == 4u);
    CHECK(offsetof(fl::CombatEventRecord, instigatorIdx) == 12u);
    CHECK(offsetof(fl::CombatEventRecord, a) == 20u);
    CHECK(offsetof(fl::CombatEventRecord, b) == 24u);
    CHECK(offsetof(fl::CombatEventRecord, c) == 28u);
    CHECK(static_cast<uint8_t>(fl::MsgId::CombatEvent) == 0x0Fu);
}

TEST_CASE("GameProtocol: CombatEventRecord round-trip through WireCodec", "[game_protocol]") {
    std::vector<uint8_t> buf;
    fl::MsgCombatEventHeader hdr;
    hdr.count = 1;
    fl::appendMsg(buf, hdr);
    fl::CombatEventRecord rec{};
    rec.type = static_cast<uint8_t>(fl::CombatEventType::Kill);
    rec.subjectIdx = 42;
    rec.subjectGen = 7;
    rec.instigatorIdx = 3;
    rec.instigatorGen = 1;
    rec.a = 0; // peer 0 gets the credit — a REAL peer id (the kNoPeer lesson)
    rec.b = fl::kNoOwningPeer;
    fl::appendMsg(buf, rec);

    fl::MsgCombatEventHeader outHdr;
    REQUIRE(fl::readMsg(buf.data(), buf.size(), outHdr));
    CHECK(outHdr.count == 1);
    fl::CombatEventRecord out;
    REQUIRE(fl::readRecordAt(buf.data(), buf.size(), sizeof(outHdr), out));
    CHECK(out.subjectIdx == 42u);
    CHECK(out.a == 0u);
    CHECK(out.b == fl::kNoOwningPeer);
}
