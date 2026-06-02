// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/GameProtocol.h"
#include "weather/WeatherTypes.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("GameProtocol: packed struct sizes match wire format", "[game_protocol]") {
    CHECK(sizeof(fl::MsgHello) == 4u);
    CHECK(sizeof(fl::MsgConnectAck) == 12u);     // extended: +assignedEntityIdx/Gen
    CHECK(sizeof(fl::MsgEntityTypeDef) == 196u); // 4 + 64 + 64 + 64
    CHECK(sizeof(fl::MsgWorldSnapshotHeader) == 12u);
    CHECK(sizeof(fl::MsgEntityEntry) == 68u);
    CHECK(sizeof(fl::MsgClientInput) == 44u);
}

TEST_CASE("GameProtocol: MsgWorldSnapshot round-trip", "[game_protocol]") {
    // Build a packet: header + 3 entity entries.
    constexpr uint16_t kCount = 3;

    fl::MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.entityCount = kCount;
    hdr.tickIndex = 42u;

    fl::MsgEntityEntry entries[kCount];
    for (uint16_t i = 0; i < kCount; ++i) {
        entries[i].entityIdx = 100u + i;
        entries[i].entityGen = 1u;
        entries[i].typeIndex = 0u;
        entries[i].pos[0] = static_cast<double>(i) * 10.0;
        entries[i].pos[1] = 500.0;
        entries[i].pos[2] = 0.0;
        entries[i].vel[0] = entries[i].vel[1] = entries[i].vel[2] = 0.0f;
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
    CHECK(parsedHdr.entityCount == kCount);
    CHECK(parsedHdr.tickIndex == 42u);

    const uint8_t* entryPtr = buf.data() + sizeof(parsedHdr);
    for (uint16_t i = 0; i < parsedHdr.entityCount; ++i) {
        fl::MsgEntityEntry e;
        std::memcpy(&e, entryPtr + i * sizeof(e), sizeof(e));
        CHECK(e.entityIdx == 100u + i);
        CHECK(e.pos[1] == 500.0);
        CHECK(e.ori[3] == 1.0f);
        CHECK(e.flags == (i == 0 ? 1u : 0u));
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
