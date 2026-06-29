// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/SnapshotCodec.h"
#include "net/WireCodec.h"
#include "weather/WeatherTypes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("GameProtocol: wire struct sizes match natural-aligned layout", "[game_protocol]") {
    CHECK(sizeof(fl::MsgHello) == 4u);
    CHECK(sizeof(fl::MsgConnectAck) == 16u);          // extended: +assignedEntityIdx/Gen, +planetRadiusKm
    CHECK(sizeof(fl::MsgEntityTypeDef) == 196u);      // 4 + 64 + 64 + 64
    CHECK(sizeof(fl::MsgWorldSnapshotHeader) == 40u); // +bitstreamBytes +frameOrigin (quantized body)
    CHECK(sizeof(fl::MsgClientInput) == 48u);
    CHECK(sizeof(fl::MsgAdminCommand) == 128u);
    CHECK(sizeof(fl::MsgAdminResponse) == 128u);
    CHECK(sizeof(fl::MsgAdminResponseChunk) == 512u);
    CHECK(sizeof(fl::MsgMotdHeader) == 4u);
    CHECK(sizeof(fl::MsgConnectRefusal) == 64u);
}

TEST_CASE("GameProtocol: wire structs are naturally aligned for zero-copy", "[game_protocol]") {
    // The snapshot header carries frameOrigin[3] (double), so it must be 8-aligned and a multiple
    // of 8 — the bitstream that follows is byte-addressed (no alignment requirement).
    CHECK(alignof(fl::MsgWorldSnapshotHeader) == 8u);
    CHECK(sizeof(fl::MsgWorldSnapshotHeader) % 8u == 0u);
    CHECK(alignof(fl::MsgClientInput) == 8u);
}

TEST_CASE("GameProtocol: MsgWorldSnapshotHeader field offsets", "[game_protocol]") {
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, recordCount) == 2u);
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, bitstreamBytes) == 4u);
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, tickIndex) == 8u);
    CHECK(offsetof(fl::MsgWorldSnapshotHeader, frameOrigin) == 16u);
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
    // The header is the only fixed struct in a snapshot; the entity body is a quantized bitstream
    // exercised by test_snapshot_codec. Here we round-trip just the header (incl. frameOrigin).
    fl::MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.recordCount = 5u;
    hdr.bitstreamBytes = 123u;
    hdr.tickIndex = 42u;
    hdr.frameOrigin[0] = 6371000.0;
    hdr.frameOrigin[1] = 500.0;
    hdr.frameOrigin[2] = -2'000'000.0;

    std::vector<uint8_t> buf(sizeof(hdr));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));

    fl::MsgWorldSnapshotHeader parsed{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), parsed));
    CHECK(parsed.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(parsed.recordCount == 5u);
    CHECK(parsed.bitstreamBytes == 123u);
    CHECK(parsed.tickIndex == 42u);
    CHECK(parsed.frameOrigin[0] == 6371000.0); // double survives exactly
    CHECK(parsed.frameOrigin[2] == -2'000'000.0);
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
    const double origin[3] = {0.0, 0.0, 0.0};
    fl::BitWriter w;
    uint32_t prev = 0;
    for (uint16_t i = 0; i < count; ++i) {
        fl::QuantEntity e;
        e.idx = 10u + i;
        e.isFull = true;
        e.gen = 1u;
        fl::encodeRecord(w, e, prev, origin, /*sendGen=*/true);
    }
    w.alignToByte();

    std::vector<uint8_t> buf;
    const std::size_t hdrOffset = buf.size();
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.tickIndex = tick;
    fl::appendMsg(buf, hdr); // placeholder
    buf.insert(buf.end(), w.bytes().begin(), w.bytes().end());
    hdr.recordCount = count;
    hdr.bitstreamBytes = static_cast<uint32_t>(w.byteCount());
    fl::writeMsgAt(buf, hdrOffset, hdr);
    return buf;
}

// Offset of the TLV block: after the header and the quantized bitstream.
std::size_t extOffsetOf(const std::vector<uint8_t>& buf) {
    fl::MsgWorldSnapshotHeader rh{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), rh));
    return sizeof(fl::MsgWorldSnapshotHeader) + rh.bitstreamBytes;
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
