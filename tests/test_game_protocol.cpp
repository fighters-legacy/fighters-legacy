// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/GameProtocol.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("GameProtocol: packed struct sizes match wire format", "[game_protocol]") {
    CHECK(sizeof(fl::MsgConnectAck) == 4u);
    CHECK(sizeof(fl::MsgEntityTypeDef) == 196u); // 4 + 64 + 64 + 64
    CHECK(sizeof(fl::MsgWorldSnapshotHeader) == 12u);
    CHECK(sizeof(fl::MsgEntityEntry) == 56u);
}

TEST_CASE("GameProtocol: MsgWorldSnapshot round-trip", "[game_protocol]") {
    // Build a packet: header + 3 entity entries.
    constexpr uint16_t kCount = 3;

    fl::MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr._pad = 0;
    hdr.entityCount = kCount;
    hdr.tickIndex = 42u;

    fl::MsgEntityEntry entries[kCount];
    for (uint16_t i = 0; i < kCount; ++i) {
        entries[i].entityIdx = 100u + i;
        entries[i].entityGen = 1u;
        entries[i].typeIndex = 0u;
        entries[i].pos[0] = static_cast<float>(i) * 10.0f;
        entries[i].pos[1] = 500.0f;
        entries[i].pos[2] = 0.0f;
        entries[i].vel[0] = entries[i].vel[1] = entries[i].vel[2] = 0.0f;
        entries[i].ori[0] = 0.0f;
        entries[i].ori[1] = 0.0f;
        entries[i].ori[2] = 0.0f;
        entries[i].ori[3] = 1.0f; // w=1 = identity
        entries[i].damageLevel = 0;
        entries[i].flags = (i == 0) ? 1u : 0u;
        entries[i]._pad[0] = entries[i]._pad[1] = 0;
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
    CHECK(parsedHdr.entityCount == kCount);
    CHECK(parsedHdr.tickIndex == 42u);

    const uint8_t* entryPtr = buf.data() + sizeof(parsedHdr);
    for (uint16_t i = 0; i < parsedHdr.entityCount; ++i) {
        fl::MsgEntityEntry e;
        std::memcpy(&e, entryPtr + i * sizeof(e), sizeof(e));
        CHECK(e.entityIdx == 100u + i);
        CHECK(e.pos[1] == 500.0f);
        CHECK(e.ori[3] == 1.0f);
        CHECK(e.flags == (i == 0 ? 1u : 0u));
    }
}

TEST_CASE("GameProtocol: MsgConnectAck round-trip with two type defs", "[game_protocol]") {
    fl::MsgConnectAck ack;
    ack.msgId = static_cast<uint8_t>(fl::MsgId::ConnectAck);
    ack.tickRateHz = 60;
    ack.typeCount = 2;

    fl::MsgEntityTypeDef defs[2]{};
    std::strncpy(defs[0].id, "builtin:debug-entity", sizeof(defs[0].id) - 1);
    std::strncpy(defs[1].id, "builtin:other", sizeof(defs[1].id) - 1);

    const std::size_t totalSize = sizeof(ack) + 2 * sizeof(fl::MsgEntityTypeDef);
    std::vector<uint8_t> buf(totalSize);
    std::memcpy(buf.data(), &ack, sizeof(ack));
    std::memcpy(buf.data() + sizeof(ack), defs, 2 * sizeof(fl::MsgEntityTypeDef));

    fl::MsgConnectAck parsedAck;
    std::memcpy(&parsedAck, buf.data(), sizeof(parsedAck));
    CHECK(parsedAck.tickRateHz == 60);
    CHECK(parsedAck.typeCount == 2);

    fl::MsgEntityTypeDef td0, td1;
    std::memcpy(&td0, buf.data() + sizeof(ack), sizeof(td0));
    std::memcpy(&td1, buf.data() + sizeof(ack) + sizeof(td0), sizeof(td1));
    CHECK(std::string_view(td0.id) == "builtin:debug-entity");
    CHECK(std::string_view(td1.id) == "builtin:other");
}
