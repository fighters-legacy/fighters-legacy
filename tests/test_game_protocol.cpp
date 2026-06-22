// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/GameProtocol.h"
#include "net/WireCodec.h"
#include "weather/WeatherTypes.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("GameProtocol: wire struct sizes match natural-aligned layout", "[game_protocol]") {
    CHECK(sizeof(fl::MsgHello) == 4u);
    CHECK(sizeof(fl::MsgConnectAck) == 16u);     // extended: +assignedEntityIdx/Gen, +planetRadiusKm
    CHECK(sizeof(fl::MsgEntityTypeDef) == 196u); // 4 + 64 + 64 + 64
    CHECK(sizeof(fl::MsgWorldSnapshotHeader) == 16u);
    CHECK(sizeof(fl::MsgEntityEntry) == 72u);
    CHECK(sizeof(fl::MsgEntityUpdate) == 52u); // compact delta record: no typeIndex, float positions
    CHECK(sizeof(fl::MsgClientInput) == 48u);
    CHECK(sizeof(fl::MsgAdminCommand) == 128u);
    CHECK(sizeof(fl::MsgAdminResponse) == 128u);
    CHECK(sizeof(fl::MsgAdminResponseChunk) == 512u);
    CHECK(sizeof(fl::MsgMotdHeader) == 4u);
    CHECK(sizeof(fl::MsgConnectRefusal) == 64u);
}

TEST_CASE("GameProtocol: wire structs are naturally aligned for zero-copy", "[game_protocol]") {
    // Records carrying a double must be 8-aligned; their sizes multiples of 8 so arrays stay aligned.
    CHECK(alignof(fl::MsgWorldSnapshotHeader) == 8u);
    CHECK(alignof(fl::MsgEntityEntry) == 8u);
    CHECK(sizeof(fl::MsgEntityEntry) % 8u == 0u);
    CHECK(alignof(fl::MsgClientInput) == 8u);
    // MsgEntityUpdate is float-aligned; 52 is a multiple of 4
    CHECK(alignof(fl::MsgEntityUpdate) == 4u);
    CHECK(sizeof(fl::MsgEntityUpdate) % 4u == 0u);
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

TEST_CASE("GameProtocol: MsgWorldSnapshot round-trip", "[game_protocol]") {
    // Build a packet: header + 3 entity entries.
    constexpr uint16_t kCount = 3;

    fl::MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.fullEntityCount = kCount;
    hdr.tickIndex = 42u;

    fl::MsgEntityEntry entries[kCount];
    for (uint16_t i = 0; i < kCount; ++i) {
        entries[i].entityIdx = 100u + i;
        entries[i].entityGen = 1u;
        entries[i].typeIndex = 0u;
        entries[i].pos[0] = static_cast<double>(i) * 10.0;
        entries[i].pos[1] = 500.0;
        entries[i].pos[2] = 0.0;
        entries[i].vel[0] = (i == 0) ? 25.5f : 0.0f;
        entries[i].vel[1] = 0.0f;
        entries[i].vel[2] = (i == 0) ? -100.f : 0.0f;
        entries[i].ori[0] = 0.0f;
        entries[i].ori[1] = 0.0f;
        entries[i].ori[2] = 0.0f;
        entries[i].ori[3] = 1.0f; // w=1 = identity
        entries[i].damageLevel = 0;
        entries[i].flags = (i == 0) ? 1u : 0u;
        entries[i].throttle = 0;
        entries[i].fuelPct = 0;
    }

    // Pack into a byte buffer (simulating network send).
    const std::size_t totalSize = sizeof(hdr) + kCount * sizeof(fl::MsgEntityEntry);
    std::vector<uint8_t> buf(totalSize);
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), entries, kCount * sizeof(fl::MsgEntityEntry));

    // Parse back using memcpy (the safe packet-parsing pattern).
    REQUIRE(buf.size() >= sizeof(fl::MsgWorldSnapshotHeader));
    fl::MsgWorldSnapshotHeader parsedHdr;
    std::memcpy(&parsedHdr, buf.data(), sizeof(parsedHdr));

    CHECK(parsedHdr.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(parsedHdr.protocolVersion == static_cast<uint8_t>(fl::kProtocolVersion));
    CHECK(parsedHdr.fullEntityCount == kCount);
    CHECK(parsedHdr.tickIndex == 42u);

    const uint8_t* entryPtr = buf.data() + sizeof(parsedHdr);
    for (uint16_t i = 0; i < parsedHdr.fullEntityCount; ++i) {
        fl::MsgEntityEntry e;
        std::memcpy(&e, entryPtr + i * sizeof(e), sizeof(e));
        CHECK(e.entityIdx == 100u + i);
        CHECK(e.pos[1] == 500.0);
        CHECK(e.ori[3] == 1.0f);
        CHECK(e.flags == (i == 0 ? 1u : 0u));
        if (i == 0) {
            CHECK(e.vel[0] == 25.5f);
            CHECK(e.vel[2] == -100.f);
        }
    }
}

TEST_CASE("GameProtocol: MsgEntityEntry double-precision round-trip at planet-scale coordinates", "[game_protocol]") {
    // At 2,000 km from origin float32 precision is ~0.24 m; double must survive exact round-trip.
    constexpr double kLargeX = 2'000'000.0;
    constexpr double kLargeZ = 2'000'000.0;

    fl::MsgEntityEntry src{};
    src.pos[0] = kLargeX;
    src.pos[1] = 500.0;
    src.pos[2] = kLargeZ;

    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgEntityEntry parsed{};
    std::memcpy(&parsed, buf.data(), sizeof(parsed));

    CHECK(parsed.pos[0] == kLargeX);
    CHECK(parsed.pos[1] == 500.0);
    CHECK(parsed.pos[2] == kLargeZ);
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

TEST_CASE("WireCodec ext: full WorldSnapshot packet with SnapshotPeerCount extension", "[game_protocol]") {
    // Build a complete snapshot (header + 2 entities) followed by a SnapshotPeerCount extension.
    std::vector<uint8_t> buf;
    const std::size_t hdrOffset = buf.size();

    fl::MsgWorldSnapshotHeader hdr{};
    hdr.tickIndex = 99u;
    fl::appendMsg(buf, hdr); // placeholder

    constexpr uint16_t kCount = 2;
    for (uint16_t i = 0; i < kCount; ++i) {
        fl::MsgEntityEntry e{};
        e.entityIdx = 10u + i;
        fl::appendMsg(buf, e);
    }
    hdr.fullEntityCount = kCount;
    fl::writeMsgAt(buf, hdrOffset, hdr);

    // Append TLV extension.
    const uint16_t kPeers = 7u;
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), kPeers);

    // Parse header and records exactly as ClientNetEventHandler does.
    fl::MsgWorldSnapshotHeader rh{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), rh));
    CHECK(rh.tickIndex == 99u);
    CHECK(rh.fullEntityCount == kCount);

    // Parse extension block.
    const std::size_t extOffset =
        sizeof(fl::MsgWorldSnapshotHeader) + static_cast<std::size_t>(rh.fullEntityCount) * sizeof(fl::MsgEntityEntry);
    REQUIRE(buf.size() > extOffset);

    uint16_t pc{};
    CHECK(fl::readExtValue(buf.data() + extOffset, buf.size() - extOffset,
                           static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
    CHECK(pc == kPeers);
}

TEST_CASE("WireCodec ext: old-receiver compatibility readMsg succeeds on extended packet", "[game_protocol]") {
    // Build the same extended snapshot and verify that old-receiver code (readMsg only) still
    // works correctly — it reads the header fields and ignores the extension bytes.
    std::vector<uint8_t> buf;
    const std::size_t hdrOffset = buf.size();

    fl::MsgWorldSnapshotHeader hdr{};
    hdr.tickIndex = 42u;
    fl::appendMsg(buf, hdr);

    fl::MsgEntityEntry e{};
    e.entityIdx = 5u;
    fl::appendMsg(buf, e);

    hdr.fullEntityCount = 1;
    fl::writeMsgAt(buf, hdrOffset, hdr);

    const uint16_t kPeers = 3u;
    fl::appendExt(buf, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), kPeers);

    // Old-receiver path: just call readMsg — succeeds, extension bytes ignored.
    fl::MsgWorldSnapshotHeader rh{};
    CHECK(fl::readMsg(buf.data(), buf.size(), rh));
    CHECK(rh.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(rh.tickIndex == 42u);
    CHECK(rh.fullEntityCount == 1u);
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

TEST_CASE("GameProtocol: MsgEntityUpdate roundtrip via appendMsg/readRecordAt", "[game_protocol]") {
    fl::MsgEntityUpdate src{};
    src.entityIdx = 7u;
    src.entityGen = 3u;
    src.damageLevel = 1u;
    src.engineFailFlags = 0x02u;
    src.pos[0] = 1234.5f;
    src.pos[1] = 500.0f;
    src.pos[2] = -999.0f;
    src.vel[0] = 10.f;
    src.vel[1] = 0.5f;
    src.vel[2] = -2.f;
    src.ori[0] = 0.f;
    src.ori[1] = 0.f;
    src.ori[2] = 0.f;
    src.ori[3] = 1.f;
    src.throttle = 75u;
    src.fuelPct = 50u;
    src.abEngaged = 1u;
    src.flags = 1u; // playerOwned

    std::vector<uint8_t> buf;
    fl::appendMsg(buf, src);
    REQUIRE(buf.size() == sizeof(fl::MsgEntityUpdate));

    fl::MsgEntityUpdate dst{};
    REQUIRE(fl::readRecordAt(buf.data(), buf.size(), 0u, dst));
    CHECK(dst.entityIdx == 7u);
    CHECK(dst.entityGen == 3u);
    CHECK(dst.damageLevel == 1u);
    CHECK(dst.engineFailFlags == 0x02u);
    CHECK(dst.pos[0] == 1234.5f);
    CHECK(dst.pos[1] == 500.0f);
    CHECK(dst.pos[2] == -999.0f);
    CHECK(dst.vel[0] == 10.f);
    CHECK(dst.throttle == 75u);
    CHECK(dst.fuelPct == 50u);
    CHECK(dst.abEngaged == 1u);
    CHECK(dst.flags == 1u);
}

TEST_CASE("GameProtocol: MsgWorldSnapshotHeader fullEntityCount and updateCount roundtrip", "[game_protocol]") {
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.fullEntityCount = 5u;
    hdr.updateCount = 3u;
    hdr.tickIndex = 42u;

    std::vector<uint8_t> buf;
    fl::appendMsg(buf, hdr);
    REQUIRE(buf.size() == sizeof(fl::MsgWorldSnapshotHeader));

    fl::MsgWorldSnapshotHeader parsed{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), parsed));
    CHECK(parsed.fullEntityCount == 5u);
    CHECK(parsed.updateCount == 3u);
    CHECK(parsed.tickIndex == 42u);
}
