// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/GameProtocol.h"
#include "net/WireCodec.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_CASE("WireCodec: readMsg copies a full struct and rejects short buffers", "[wire_codec]") {
    fl::MsgHello src{};
    src.protocolVersion = 7u;
    std::vector<uint8_t> buf(sizeof(src));
    std::memcpy(buf.data(), &src, sizeof(src));

    fl::MsgHello out{};
    CHECK(fl::readMsg(buf.data(), buf.size(), out));
    CHECK(out.protocolVersion == 7u);

    // One byte short: refused, out untouched.
    fl::MsgHello out2{};
    out2.protocolVersion = 99u;
    CHECK_FALSE(fl::readMsg(buf.data(), sizeof(src) - 1, out2));
    CHECK(out2.protocolVersion == 99u);

    // Empty buffer.
    CHECK_FALSE(fl::readMsg(buf.data(), 0u, out2));
}

TEST_CASE("WireCodec: readRecordAt bounds-checks variable-length tails", "[wire_codec]") {
    // Header + two records, parsed like MsgConnectAck + MsgEntityTypeDef[].
    fl::MsgConnectAck ack{};
    ack.typeCount = 2;
    fl::MsgEntityTypeDef defs[2]{};
    std::snprintf(defs[0].id, sizeof(defs[0].id), "%s", "a");
    std::snprintf(defs[1].id, sizeof(defs[1].id), "%s", "b");

    std::vector<uint8_t> buf;
    fl::appendMsg(buf, ack);
    fl::appendMsg(buf, defs[0]);
    fl::appendMsg(buf, defs[1]);

    std::size_t off = sizeof(ack);
    fl::MsgEntityTypeDef r0{}, r1{};
    REQUIRE(fl::readRecordAt(buf.data(), buf.size(), off, r0));
    off += sizeof(r0);
    REQUIRE(fl::readRecordAt(buf.data(), buf.size(), off, r1));
    off += sizeof(r1);
    CHECK(std::string_view(r0.id) == "a");
    CHECK(std::string_view(r1.id) == "b");

    // A third record would read past the end -> refused.
    fl::MsgEntityTypeDef r2{};
    CHECK_FALSE(fl::readRecordAt(buf.data(), buf.size(), off, r2));

    // Truncated last record (drop one byte) -> refused, not a partial read.
    CHECK_FALSE(fl::readRecordAt(buf.data(), buf.size() - 1, sizeof(ack) + sizeof(r0), r2));

    // Offset past end -> refused (no overflow).
    CHECK_FALSE(fl::readRecordAt(buf.data(), buf.size(), buf.size() + 100u, r2));
}

TEST_CASE("WireCodec: viewMsg returns a pointer when aligned, nullptr when not", "[wire_codec]") {
    // MsgEntityEntry has alignof 8 (it carries a double). Use an over-aligned backing buffer so the
    // alignment behaviour is deterministic across platforms.
    alignas(8) std::byte storage[sizeof(fl::MsgEntityEntry) + 8]{};
    fl::MsgEntityEntry src{};
    src.entityIdx = 4242u;
    src.pos[1] = 500.0;
    std::memcpy(storage, &src, sizeof(src));

    // Aligned base: view succeeds and reads the right value with no copy.
    const fl::MsgEntityEntry* v = fl::viewMsg<fl::MsgEntityEntry>(storage, sizeof(src));
    REQUIRE(v != nullptr);
    CHECK(v->entityIdx == 4242u);
    CHECK(v->pos[1] == 500.0);

    // Misaligned pointer (base + 1): view declines, caller would fall back to readMsg.
    CHECK(fl::viewMsg<fl::MsgEntityEntry>(storage + 1, sizeof(src)) == nullptr);

    // Too small: declines regardless of alignment.
    CHECK(fl::viewMsg<fl::MsgEntityEntry>(storage, sizeof(src) - 1) == nullptr);
}

TEST_CASE("WireCodec: appendMsg + writeMsgAt build a snapshot the reader round-trips", "[wire_codec]") {
    // Mirror the server snapshot builder: placeholder header, append entries, patch the count back in.
    std::vector<uint8_t> buf;
    const std::size_t hdrOffset = buf.size();
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.tickIndex = 7u;
    fl::appendMsg(buf, hdr); // placeholder

    constexpr uint16_t kCount = 3;
    for (uint16_t i = 0; i < kCount; ++i) {
        fl::MsgEntityEntry e{};
        e.entityIdx = 100u + i;
        e.pos[0] = static_cast<double>(i) * 10.0;
        fl::appendMsg(buf, e);
    }
    hdr.entityCount = kCount;
    fl::writeMsgAt(buf, hdrOffset, hdr);

    // Read back exactly like ClientNetEventHandler.
    fl::MsgWorldSnapshotHeader rh{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), rh));
    CHECK(rh.entityCount == kCount);
    CHECK(rh.tickIndex == 7u);

    std::size_t off = sizeof(rh);
    for (uint16_t i = 0; i < rh.entityCount; ++i) {
        fl::MsgEntityEntry e{};
        REQUIRE(fl::readRecordAt(buf.data(), buf.size(), off, e));
        off += sizeof(e);
        CHECK(e.entityIdx == 100u + i);
        CHECK(e.pos[0] == static_cast<double>(i) * 10.0);
    }
}
