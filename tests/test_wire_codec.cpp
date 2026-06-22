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

TEST_CASE("WireCodec ext: single extension round-trip via appendExt and readExtValue", "[wire_codec]") {
    std::vector<uint8_t> buf;
    const uint32_t kVal = 0xDEADBEEFu;
    fl::appendExt(buf, 0x0100u, kVal);

    // 4-byte TLV header + 4-byte uint32_t
    REQUIRE(buf.size() == 8u);

    uint32_t out{};
    CHECK(fl::readExtValue(buf.data(), buf.size(), 0x0100u, out));
    CHECK(out == kVal);
}

TEST_CASE("WireCodec ext: findExt returns nullptr for absent tag", "[wire_codec]") {
    std::vector<uint8_t> buf;
    const uint16_t kPresent = 42u;
    fl::appendExt(buf, 0x0100u, kPresent);

    uint16_t valueLen{};
    CHECK(fl::findExt(buf.data(), buf.size(), 0x0101u, valueLen) == nullptr);
}

TEST_CASE("WireCodec ext: multiple extensions skip unknown tag to find known one", "[wire_codec]") {
    std::vector<uint8_t> buf;
    const uint32_t kFirst = 11u;
    const uint16_t kSecond = 22u;
    fl::appendExt(buf, 0x0101u, kFirst);  // not the target
    fl::appendExt(buf, 0x0100u, kSecond); // the target

    uint16_t out{};
    CHECK(fl::readExtValue(buf.data(), buf.size(), 0x0100u, out));
    CHECK(out == kSecond);

    // First tag must also be found independently.
    uint32_t first{};
    CHECK(fl::readExtValue(buf.data(), buf.size(), 0x0101u, first));
    CHECK(first == kFirst);
}

TEST_CASE("WireCodec ext: malformed extension block truncated handled safely", "[wire_codec]") {
    // Manually craft: tag=0x0100, len=10 but only 2 data bytes present.
    std::vector<uint8_t> buf;
    const uint16_t tag = 0x0100u;
    const uint16_t len = 10u;
    buf.resize(4 + 2); // header + 2 data bytes (not 10)
    std::memcpy(buf.data(), &tag, 2);
    std::memcpy(buf.data() + 2, &len, 2);
    buf[4] = 0xAA;
    buf[5] = 0xBB;

    uint16_t valueLen{};
    CHECK(fl::findExt(buf.data(), buf.size(), 0x0100u, valueLen) == nullptr);
}

TEST_CASE("WireCodec ext: appendExtRaw and findExt raw bytes round-trip", "[wire_codec]") {
    const uint8_t kBytes[3] = {0x11, 0x22, 0x33};
    std::vector<uint8_t> buf;
    fl::appendExtRaw(buf, 0x0200u, kBytes, 3u);

    REQUIRE(buf.size() == 7u); // 4-byte header + 3 bytes

    uint16_t valueLen{};
    const uint8_t* p = fl::findExt(buf.data(), buf.size(), 0x0200u, valueLen);
    REQUIRE(p != nullptr);
    CHECK(valueLen == 3u);
    CHECK(p[0] == 0x11);
    CHECK(p[1] == 0x22);
    CHECK(p[2] == 0x33);
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
    hdr.fullEntityCount = kCount;
    fl::writeMsgAt(buf, hdrOffset, hdr);

    // Read back exactly like ClientNetEventHandler.
    fl::MsgWorldSnapshotHeader rh{};
    REQUIRE(fl::readMsg(buf.data(), buf.size(), rh));
    CHECK(rh.fullEntityCount == kCount);
    CHECK(rh.tickIndex == 7u);

    std::size_t off = sizeof(rh);
    for (uint16_t i = 0; i < rh.fullEntityCount; ++i) {
        fl::MsgEntityEntry e{};
        REQUIRE(fl::readRecordAt(buf.data(), buf.size(), off, e));
        off += sizeof(e);
        CHECK(e.entityIdx == 100u + i);
        CHECK(e.pos[0] == static_cast<double>(i) * 10.0);
    }
}
