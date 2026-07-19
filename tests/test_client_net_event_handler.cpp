// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ClientNetEventHandler.h"
#include "ServerNotice.h"

#include "ILogger.h"
#include "INetwork.h"
#include "RenderTypes.h"
#include "console/CommandRegistry.h"
#include "console/GameConsole.h"
#include "entity/EntityTypeRegistry.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/SnapshotCodec.h"
#include "net/SnapshotCompression.h"
#include "net/SnapshotScheduler.h" // kSnapshotRetentionTicks
#include "net/WireCodec.h"

#include "SessionStatus.h"
#include "render/SimRenderBridge.h"

#include "mock_network.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

namespace {

// One entity record for the quantized snapshot builder below. Field names mirror the wire fields.
struct TestRec {
    uint32_t idx{0};
    uint32_t gen{0};
    uint32_t typeIndex{0};
    bool isFull{false};   // full record: carries typeIndex + gen
    bool sendGen{false};  // force the genPresent bit on a delta (e.g. to test the stale-gen guard)
    bool hasOmega{false}; // own-entity omega
    double pos[3]{};
    float vel[3]{};
    float quat[4]{0.f, 0.f, 0.f, 1.f};
    float omega[3]{};
    uint8_t damage{0};
    uint8_t engineFail{0};
    uint8_t throttle{0};
    uint8_t fuel{0};
    bool ab{false};
    bool owned{false};
};

// Build a MsgWorldSnapshot packet (header + quantized bitstream) from a list of records. Append a
// TLV block afterwards if needed. Mirrors WorldBroadcaster's encode path.
inline std::vector<uint8_t> buildSnapshotPkt(uint64_t tick, const std::vector<TestRec>& recs,
                                             std::array<double, 3> origin = {0.0, 0.0, 0.0}) {
    // All records reference a single shared origin at table index 0 (#725 encode-once layout).
    std::vector<uint8_t> stream;
    for (const auto& rrec : recs) {
        fl::QuantEntity qe;
        qe.idx = rrec.idx;
        qe.gen = rrec.gen;
        qe.typeIndex = rrec.typeIndex;
        qe.isFull = rrec.isFull;
        qe.hasOmega = rrec.hasOmega;
        for (int i = 0; i < 3; ++i) {
            qe.pos[i] = rrec.pos[i];
            qe.vel[i] = rrec.vel[i];
            qe.omega[i] = rrec.omega[i];
        }
        for (int i = 0; i < 4; ++i)
            qe.quat[i] = rrec.quat[i];
        qe.damageLevel = rrec.damage;
        qe.engineFailFlags = rrec.engineFail;
        qe.throttle = rrec.throttle;
        qe.fuelPct = rrec.fuel;
        qe.abEngaged = rrec.ab;
        qe.playerOwned = rrec.owned;
        std::vector<uint8_t> blob;
        fl::encodeStandaloneRecord(blob, qe, origin.data(), /*sendGen=*/rrec.isFull || rrec.sendGen);
        fl::appendStitchedRecord(stream, /*originIndex=*/0u, blob);
    }

    std::vector<uint8_t> buf;
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.tickIndex = tick;
    const std::size_t hdrOffset = buf.size();
    fl::appendMsg(buf, hdr);
    // Origin table: one entry (all records reference index 0).
    const auto* op = reinterpret_cast<const uint8_t*>(origin.data());
    buf.insert(buf.end(), op, op + 3u * sizeof(double));
    buf.insert(buf.end(), stream.begin(), stream.end());
    hdr.recordCount = static_cast<uint16_t>(recs.size());
    hdr.originCount = 1u;
    hdr.bitstreamBytes = static_cast<uint32_t>(stream.size());
    fl::writeMsgAt(buf, hdrOffset, hdr);
    return buf;
}

struct MockLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Records disconnect()/disconnectPeer() so the version-mismatch and refusal paths are assertable.
using MockNetwork = TrackingNetwork;

// Build a raw MsgMotd packet: MsgMotdHeader + text + NUL terminator.
static std::vector<uint8_t> makeMotdPacket(std::string_view text, uint16_t displaySeconds = 0) {
    fl::MsgMotdHeader hdr{};
    hdr.displaySeconds = displaySeconds;
    std::vector<uint8_t> pkt;
    fl::appendMsg(pkt, hdr);
    pkt.insert(pkt.end(), text.begin(), text.end());
    pkt.push_back(0u);
    return pkt;
}

} // namespace

TEST_CASE("ClientNetEventHandler: MsgMotd single-line text shown in notice", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ServerNotice notice;

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;

    auto pkt = makeMotdPacket("Hello");
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto elems = notice.buildElements();
    REQUIRE(!elems.empty());
    CHECK(std::string(elems[0].text) == "[server] Hello");
}

TEST_CASE("ClientNetEventHandler: MsgMotd multi-line; notice receives first line only", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ServerNotice notice;

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;

    auto pkt = makeMotdPacket("Line1\nLine2");
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto elems = notice.buildElements();
    REQUIRE(!elems.empty());
    CHECK(std::string(elems[0].text) == "[server] Line1");
}

TEST_CASE("ClientNetEventHandler: MsgMotd CRLF line ending stripped", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ServerNotice notice;

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;

    auto pkt = makeMotdPacket("Hi\r\nBye");
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto elems = notice.buildElements();
    REQUIRE(!elems.empty());
    CHECK(std::string(elems[0].text) == "[server] Hi");
}

TEST_CASE("ClientNetEventHandler: MsgMotd packet too small does not set notice", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ServerNotice notice;

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;

    // 1 byte: msgId only
    const uint8_t pkt1[] = {static_cast<uint8_t>(fl::MsgId::Motd)};
    handler.onReceive(0u, pkt1, sizeof(pkt1));
    CHECK(notice.buildElements().empty());

    // 2 bytes: msgId + one byte of displaySeconds
    const uint8_t pkt2[] = {static_cast<uint8_t>(fl::MsgId::Motd), 0x00};
    handler.onReceive(0u, pkt2, sizeof(pkt2));
    CHECK(notice.buildElements().empty());

    // 3 bytes: msgId + displaySeconds but no NUL terminator
    const uint8_t pkt3[] = {static_cast<uint8_t>(fl::MsgId::Motd), 0x00, 0x00};
    handler.onReceive(0u, pkt3, sizeof(pkt3));
    CHECK(notice.buildElements().empty());
}

TEST_CASE("ClientNetEventHandler: MsgMissionRoster populates the object-id -> entity map (#914)",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Build a two-record roster packet the way WorldBroadcaster sends it.
    std::vector<uint8_t> pkt;
    fl::MsgMissionRoster a{};
    a.entityIdx = 11;
    a.entityGen = 2;
    std::snprintf(a.objectId, sizeof(a.objectId), "%s", "bandit1");
    fl::MsgMissionRoster b{};
    b.entityIdx = 5;
    b.entityGen = 9;
    std::snprintf(b.objectId, sizeof(b.objectId), "%s", "player1");
    fl::appendMsg(pkt, a);
    fl::appendMsg(pkt, b);

    handler.onReceive(0u, pkt.data(), pkt.size());

    uint32_t idx = 0;
    uint16_t gen = 0;
    REQUIRE(handler.missionEntity("bandit1", idx, gen));
    CHECK(idx == 11u);
    CHECK(gen == 2u);
    REQUIRE(handler.missionEntity("player1", idx, gen));
    CHECK(idx == 5u);
    CHECK(gen == 9u);
    CHECK_FALSE(handler.missionEntity("ghost", idx, gen));

    // A single-record delta (late player-slot bind) updates the mapping in place.
    std::vector<uint8_t> delta;
    fl::MsgMissionRoster upd{};
    upd.entityIdx = 77;
    upd.entityGen = 4;
    std::snprintf(upd.objectId, sizeof(upd.objectId), "%s", "player1");
    fl::appendMsg(delta, upd);
    handler.onReceive(0u, delta.data(), delta.size());
    REQUIRE(handler.missionEntity("player1", idx, gen));
    CHECK(idx == 77u);
    CHECK(gen == 4u);
}

TEST_CASE("ClientNetEventHandler: unknown msgId discarded, no notice", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ServerNotice notice;

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;

    const uint8_t pkt[] = {0x0A, 'x', 0x00};
    handler.onReceive(0u, pkt, sizeof(pkt));

    CHECK(notice.buildElements().empty());
}

TEST_CASE("ClientNetEventHandler: MsgMotd notice auto-dismisses after 15 seconds", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    fl::ManualClock fakeTime;
    ServerNotice notice;
    notice.setClock(fakeTime);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;

    auto pkt = makeMotdPacket("Welcome to the server");
    handler.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(!notice.buildElements().empty());

    fakeTime.advance(std::chrono::seconds(16));
    CHECK(notice.buildElements().empty());
}

TEST_CASE("ClientNetEventHandler: MsgMotd single-line text printed to console", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdReg;
    GameConsole console(logger, cmdReg);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    auto pkt = makeMotdPacket("Hello");
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "[server] Hello");
}

TEST_CASE("ClientNetEventHandler: MsgMotd multi-line text each line printed to console", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdReg;
    GameConsole console(logger, cmdReg);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    auto pkt = makeMotdPacket("Line1\nLine2\nLine3");
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "[server] Line1");
    CHECK(lines[1] == "[server] Line2");
    CHECK(lines[2] == "[server] Line3");
}

TEST_CASE("ClientNetEventHandler: MsgWorldSnapshot abEngaged and engineFailFlags unpacked correctly",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Build a WorldSnapshot with one full record carrying new telemetry fields. omega is only on the
    // wire for an own-entity record (hasOmega), so mark it as such.
    TestRec rec;
    rec.idx = 0u;
    rec.gen = 1u;
    rec.isFull = true;
    rec.hasOmega = true;
    rec.ab = true;
    rec.engineFail = fl::kEngineFailGeneric;
    rec.omega[0] = 0.1f;
    rec.omega[1] = 0.2f;
    rec.omega[2] = 0.3f;
    auto pkt = buildSnapshotPkt(1u, {rec});

    handler.onReceive(0u, pkt.data(), pkt.size());

    bridge.tryAdvance();
    const auto& snap = bridge.current();
    REQUIRE(snap.entries.size() == 1u);
    CHECK(snap.entries[0].abEngaged == true);
    CHECK(snap.entries[0].engineFailFlags == fl::kEngineFailGeneric);
    CHECK(snap.entries[0].omega.x == Catch::Approx(0.1f).margin(0.02));
    CHECK(snap.entries[0].omega.y == Catch::Approx(0.2f).margin(0.02));
    CHECK(snap.entries[0].omega.z == Catch::Approx(0.3f).margin(0.02));
}

// Compress a raw snapshot packet into its #775 wire form: zstd the payload after the 24-byte
// header, patch flags + uncompressedBytes. Requires the payload to be big enough to compress
// (callers build multi-record packets).
static std::vector<uint8_t> compressSnapshotPacket(const std::vector<uint8_t>& raw) {
    const std::size_t hdrSize = sizeof(fl::MsgWorldSnapshotHeader);
    REQUIRE(raw.size() > hdrSize);
    std::vector<uint8_t> z;
    const std::size_t csz = fl::compressSnapshotPayload(raw.data() + hdrSize, raw.size() - hdrSize, z);
    REQUIRE(csz > 0u);
    std::vector<uint8_t> pkt(raw.begin(), raw.begin() + hdrSize);
    fl::MsgWorldSnapshotHeader hdr{};
    std::memcpy(&hdr, pkt.data(), hdrSize);
    hdr.flags |= fl::kSnapshotFlagCompressed;
    hdr.uncompressedBytes = static_cast<uint32_t>(raw.size() - hdrSize);
    std::memcpy(pkt.data(), &hdr, hdrSize);
    pkt.insert(pkt.end(), z.begin(), z.end());
    return pkt;
}

// Enough similar records to clear kMinSnapshotCompressBytes so the compressed path is exercised.
static std::vector<TestRec> manyRecords(int n) {
    std::vector<TestRec> recs;
    for (int i = 0; i < n; ++i) {
        TestRec rec;
        rec.idx = static_cast<uint32_t>(i);
        rec.gen = 1u;
        rec.isFull = true;
        rec.pos[0] = 100.0 * i;
        rec.pos[1] = 500.0;
        recs.push_back(rec);
    }
    return recs;
}

TEST_CASE("ClientNetEventHandler: compressed WorldSnapshot decodes like the raw one",
          "[client_net_event_handler][compress]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    const auto raw = buildSnapshotPkt(1u, manyRecords(8));
    const auto pkt = compressSnapshotPacket(raw);
    REQUIRE(pkt.size() < raw.size());

    handler.onReceive(0u, pkt.data(), pkt.size());

    bridge.tryAdvance();
    const auto& snap = bridge.current();
    REQUIRE(snap.entries.size() == 8u);
    CHECK(snap.tickIndex == 1u);
    // The rendered set is rebuilt from an unordered cache — locate by entityIdx, not position.
    const auto it = std::find_if(snap.entries.begin(), snap.entries.end(),
                                 [](const fl::EntityRenderEntry& e) { return e.entityIdx == 3u; });
    REQUIRE(it != snap.entries.end());
    CHECK(it->position.x == Catch::Approx(300.0).margin(fl::kPosStepM));
}

TEST_CASE("ClientNetEventHandler: malformed compressed snapshots are dropped fail-closed",
          "[client_net_event_handler][compress]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    const auto raw = buildSnapshotPkt(1u, manyRecords(8));
    const auto good = compressSnapshotPacket(raw);
    const std::size_t hdrSize = sizeof(fl::MsgWorldSnapshotHeader);

    SECTION("oversized uncompressedBytes claim") {
        auto pkt = good;
        fl::MsgWorldSnapshotHeader hdr{};
        std::memcpy(&hdr, pkt.data(), hdrSize);
        hdr.uncompressedBytes = static_cast<uint32_t>(fl::kMaxSnapshotPayloadBytes + 1u);
        std::memcpy(pkt.data(), &hdr, hdrSize);
        handler.onReceive(0u, pkt.data(), pkt.size());
    }
    SECTION("claim does not match the frame") {
        auto pkt = good;
        fl::MsgWorldSnapshotHeader hdr{};
        std::memcpy(&hdr, pkt.data(), hdrSize);
        hdr.uncompressedBytes += 1u;
        std::memcpy(pkt.data(), &hdr, hdrSize);
        handler.onReceive(0u, pkt.data(), pkt.size());
    }
    SECTION("garbage zstd frame") {
        auto pkt = good;
        for (std::size_t i = hdrSize; i < pkt.size(); ++i)
            pkt[i] = 0xA5u;
        handler.onReceive(0u, pkt.data(), pkt.size());
    }
    SECTION("flag set with no payload at all") {
        std::vector<uint8_t> pkt(good.begin(), good.begin() + hdrSize);
        handler.onReceive(0u, pkt.data(), pkt.size());
    }

    // Every malformed form must be dropped without publishing anything (and without crashing).
    CHECK_FALSE(bridge.tryAdvance());
}

TEST_CASE("ClientNetEventHandler: out-of-order WorldSnapshot is ignored", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Newer snapshot (tick 5) places the entity at x=500; a delayed older snapshot (tick 3) would
    // place it at x=100. The older one must be dropped so it can't clobber newer state, keeping the
    // tick echoed to the server (the ack) monotonic.
    TestRec newer;
    newer.idx = 0u;
    newer.gen = 1u;
    newer.isFull = true;
    newer.pos[0] = 500.0;
    auto pktNew = buildSnapshotPkt(5u, {newer});

    TestRec older = newer;
    older.pos[0] = 100.0;
    auto pktOld = buildSnapshotPkt(3u, {older});

    handler.onReceive(0u, pktNew.data(), pktNew.size());
    handler.onReceive(0u, pktOld.data(), pktOld.size());

    bridge.tryAdvance();
    const auto& snap = bridge.current();
    REQUIRE(snap.entries.size() == 1u);
    CHECK(snap.tickIndex == 5u); // older tick 3 was not applied
    CHECK(snap.entries[0].position.x == Catch::Approx(500.0).margin(fl::kPosStepM));
}

TEST_CASE("ClientNetEventHandler: first WorldSnapshot at tick 0 is processed", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // The out-of-order guard must not drop the legitimate first snapshot just because its tick is 0.
    TestRec rec;
    rec.idx = 0u;
    rec.gen = 1u;
    rec.isFull = true;
    auto pkt = buildSnapshotPkt(0u, {rec});
    handler.onReceive(0u, pkt.data(), pkt.size());

    bridge.tryAdvance();
    CHECK(bridge.current().entries.size() == 1u);
}

TEST_CASE("ClientNetEventHandler: MsgMotd honours custom motdDisplaySeconds", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    fl::ManualClock fakeTime;
    ServerNotice notice;
    notice.setClock(fakeTime);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;
    handler.motdDisplaySeconds = 5;

    auto pkt = makeMotdPacket("Custom duration");
    handler.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(!notice.buildElements().empty());

    fakeTime.advance(std::chrono::seconds(4));
    CHECK(!notice.buildElements().empty()); // still within window

    fakeTime.advance(std::chrono::seconds(2));
    CHECK(notice.buildElements().empty()); // expired at 6 s
}

TEST_CASE("ClientNetEventHandler: MsgMotd motdDisplaySeconds 0 is persistent", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    fl::ManualClock fakeTime;
    ServerNotice notice;
    notice.setClock(fakeTime);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;
    handler.motdDisplaySeconds = 0;

    auto pkt = makeMotdPacket("Persistent notice");
    handler.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(!notice.buildElements().empty());

    fakeTime.advance(std::chrono::seconds(3600));
    CHECK(!notice.buildElements().empty()); // still shown — no expiry set
}

TEST_CASE("ClientNetEventHandler: MsgMotd server displaySeconds overrides client motdDisplaySeconds",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    fl::ManualClock fakeTime;
    ServerNotice notice;
    notice.setClock(fakeTime);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;
    handler.motdDisplaySeconds = 15; // client default

    auto pkt = makeMotdPacket("Rules", 30); // server requests 30 s
    handler.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(!notice.buildElements().empty());

    fakeTime.advance(std::chrono::seconds(29));
    CHECK(!notice.buildElements().empty()); // still within server window

    fakeTime.advance(std::chrono::seconds(2));
    CHECK(notice.buildElements().empty()); // expired at 31 s (past 30 s server window)
}

// ---------------------------------------------------------------------------
// Failure signaling tests (connectFailMsg atomic)
// ---------------------------------------------------------------------------

namespace {

static std::vector<uint8_t> makeMsgHello(uint8_t protocolVersion) {
    fl::MsgHello msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::Hello);
    msg.protocolVersion = protocolVersion;
    std::vector<uint8_t> pkt(sizeof(msg));
    std::memcpy(pkt.data(), &msg, sizeof(msg));
    return pkt;
}

static std::vector<uint8_t> makeRefusalPacket(fl::ConnectRefusalCode code) {
    fl::MsgConnectRefusal msg{};
    msg.code = static_cast<uint8_t>(code);
    std::vector<uint8_t> pkt(sizeof(msg));
    std::memcpy(pkt.data(), &msg, sizeof(msg));
    return pkt;
}

} // namespace

TEST_CASE("ClientNetEventHandler: MsgHello version mismatch sets atomic and disconnects",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    handler.onConnect(0u);
    auto pkt = makeMsgHello(static_cast<uint8_t>(fl::kProtocolVersion) ^ 0xFF);
    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.disconnectCount == 1);
    CHECK(failMsg.load() == SessionFailure::VersionMismatch);
}

TEST_CASE("ClientNetEventHandler: ENet rejection sets connection refused via onDisconnect",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    handler.onConnect(0u); // m_connected = true, assignedEntityIdx still 0
    handler.onDisconnect(0u);

    CHECK(failMsg.load() == SessionFailure::ConnectionRefused);
}

TEST_CASE("ClientNetEventHandler: version mismatch message not overwritten by onDisconnect CAS",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    handler.onConnect(0u);
    auto pkt = makeMsgHello(static_cast<uint8_t>(fl::kProtocolVersion) ^ 0xFF);
    handler.onReceive(0u, pkt.data(), pkt.size()); // sets "Server version mismatch."
    handler.onDisconnect(0u);                      // CAS should fail — already set

    CHECK(failMsg.load() == SessionFailure::VersionMismatch);
}

TEST_CASE("ClientNetEventHandler: onDisconnect does not signal after a successful ConnectAck (#853)",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    handler.onConnect(0u);
    // A ConnectAck marks the connection as established. The rejection sentinel now keys on "did an ack
    // arrive", not assignedEntityIdx==0, so a mid-flight disconnect is not reported as a rejection.
    fl::MsgConnectAck ack{};
    ack.assignedEntityIdx = 1u;
    ack.assignedEntityGen = 1u;
    ack.grantedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    std::vector<uint8_t> pkt(sizeof(ack));
    std::memcpy(pkt.data(), &ack, sizeof(ack));
    handler.onReceive(0u, pkt.data(), pkt.size());
    handler.onDisconnect(0u);

    CHECK(failMsg.load() == SessionFailure::None);
}

TEST_CASE("ClientNetEventHandler: an observer's entity-less ack is not a rejection (#857)",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    handler.onConnect(0u);
    // Observer ack: assignedEntityGen == 0 (no entity) but grantedRole == Observer. This must count as
    // a successful connect — the old assignedEntityIdx==0 sentinel would have misread it as a rejection.
    fl::MsgConnectAck ack{};
    ack.assignedEntityIdx = 0u;
    ack.assignedEntityGen = 0u;
    ack.grantedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    std::vector<uint8_t> pkt(sizeof(ack));
    std::memcpy(pkt.data(), &ack, sizeof(ack));
    handler.onReceive(0u, pkt.data(), pkt.size());
    CHECK(handler.grantedRole() == fl::PeerRole::Observer);
    handler.onDisconnect(0u);
    CHECK(failMsg.load() == SessionFailure::None);
}

TEST_CASE("ClientNetEventHandler: null connectFailMsg does not crash on rejection", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    // connectFailMsg deliberately not set (default nullptr)

    handler.onConnect(0u);
    handler.onDisconnect(0u); // must not crash
}

TEST_CASE("ClientNetEventHandler: ENet timeout path does not signal when onConnect never fired",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    // No onConnect() call — simulates ENet timeout (server unreachable)
    handler.onDisconnect(0u);

    CHECK(failMsg.load() == SessionFailure::None);
}

TEST_CASE("ClientNetEventHandler: correct protocolVersion does not disconnect or signal failure",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    handler.onConnect(0u);
    auto pkt = makeMsgHello(static_cast<uint8_t>(fl::kProtocolVersion));
    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.disconnectCount == 0);
    CHECK(failMsg.load() == SessionFailure::None);
}

// ---------------------------------------------------------------------------
// MsgConnectRefusal tests
// ---------------------------------------------------------------------------

TEST_CASE("ClientNetEventHandler: MsgConnectRefusal sets connectFailMsg with reason from wire",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    auto pkt = makeRefusalPacket(fl::ConnectRefusalCode::Banned);
    handler.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(failMsg.load() != SessionFailure::None);
    CHECK(failMsg.load() == SessionFailure::Banned);
}

TEST_CASE("ClientNetEventHandler: MsgConnectRefusal reason not overwritten by onDisconnect CAS",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    handler.onConnect(0u);
    auto pkt = makeRefusalPacket(fl::ConnectRefusalCode::AccessDenied);
    handler.onReceive(0u, pkt.data(), pkt.size()); // sets specific reason
    handler.onDisconnect(0u);                      // CAS should fail — already set

    CHECK(failMsg.load() == SessionFailure::AccessDenied);
}

TEST_CASE("ClientNetEventHandler: MsgConnectRefusal packet too small does not set connectFailMsg",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;

    // 3 bytes is far smaller than sizeof(MsgConnectRefusal) = 64
    const uint8_t pkt[] = {static_cast<uint8_t>(fl::MsgId::ConnectRefusal), 0x00, 0x00};
    handler.onReceive(0u, pkt, sizeof(pkt));

    CHECK(failMsg.load() == SessionFailure::None);
}

TEST_CASE("ClientNetEventHandler: MsgConnectRefusal with null connectFailMsg does not crash",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    // connectFailMsg deliberately not set (default nullptr)

    auto pkt = makeRefusalPacket(fl::ConnectRefusalCode::TooManyConnections);
    handler.onReceive(0u, pkt.data(), pkt.size()); // must not crash
}

TEST_CASE("ClientNetEventHandler: MsgMotd server displaySeconds 0 falls back to client motdDisplaySeconds",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    fl::ManualClock fakeTime;
    ServerNotice notice;
    notice.setClock(fakeTime);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;
    handler.motdDisplaySeconds = 5; // client prefers 5 s

    auto pkt = makeMotdPacket("Hi", 0); // server defers to client
    handler.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(!notice.buildElements().empty());

    fakeTime.advance(std::chrono::seconds(4));
    CHECK(!notice.buildElements().empty()); // within client window

    fakeTime.advance(std::chrono::seconds(2));
    CHECK(notice.buildElements().empty()); // expired at 6 s (past 5 s client window)
}

TEST_CASE("ClientNetEventHandler: MsgConnectAck planetRadiusKm parsed correctly", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    fl::MsgConnectAck ack{};
    ack.planetRadiusKm = 6371.f;
    handler.onConnect(0u);
    handler.onReceive(0u, &ack, sizeof(ack));

    CHECK(handler.planetRadiusKm() == Catch::Approx(6371.f).epsilon(1e-4f));
}

TEST_CASE("ClientNetEventHandler: short MsgConnectAck (12 bytes) does not crash", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // 12-byte ack (old wire format without planetRadiusKm)
    uint8_t buf[12]{};
    buf[0] = static_cast<uint8_t>(fl::MsgId::ConnectAck);
    handler.onConnect(0u);
    REQUIRE_NOTHROW(handler.onReceive(0u, buf, sizeof(buf)));
    // planetRadiusKm stays at the Earth default when short packet is received
    CHECK(handler.planetRadiusKm() == Catch::Approx(6371.f).epsilon(1e-4f));
}

// ---------------------------------------------------------------------------
// MsgAdminResponseChunk reassembly tests
// ---------------------------------------------------------------------------

namespace {

static std::vector<uint8_t> makeChunkPacket(uint16_t reqId, uint16_t seq, uint8_t flags, std::string_view body) {
    fl::MsgAdminResponseChunk chunk{};
    chunk.reqId = reqId;
    chunk.seqNum = seq;
    chunk.flags = flags;
    std::size_t n = std::min(body.size(), fl::kAdminChunkPayload);
    std::memcpy(chunk.body, body.data(), n);
    chunk.body[n] = '\0';
    return {reinterpret_cast<const uint8_t*>(&chunk), reinterpret_cast<const uint8_t*>(&chunk) + sizeof(chunk)};
}

} // namespace

TEST_CASE("ClientNetEventHandler: single chunk with kChunkFlagEnd prints to console",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    auto pkt = makeChunkPacket(1u, 0u, fl::kChunkFlagEnd, "hello");
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 1u);
    CHECK(lines[0] == "[admin] hello");
}

TEST_CASE("ClientNetEventHandler: two chunks assembled before printing to console",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    // First chunk — no end flag, nothing printed yet.
    auto pkt0 = makeChunkPacket(1u, 0u, 0u, "foo");
    handler.onReceive(0u, pkt0.data(), pkt0.size());
    CHECK(console.outputLines().empty());

    // Second chunk — end flag, assembled string printed.
    auto pkt1 = makeChunkPacket(1u, 1u, fl::kChunkFlagEnd, "bar");
    handler.onReceive(0u, pkt1.data(), pkt1.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 1u);
    CHECK(lines[0] == "[admin] foobar");
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponse fast-path still prints to console",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    fl::MsgAdminResponse resp{};
    resp.reqId = 7u;
    std::snprintf(resp.text, sizeof(resp.text), "pong");
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&resp),
                                    reinterpret_cast<const uint8_t*>(&resp) + sizeof(resp));
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 1u);
    CHECK(lines[0] == "[admin] pong");
}

TEST_CASE("ClientNetEventHandler: oversized chunk stream dropped gracefully",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    // Send chunks (without end flag) totalling more than 64 KB.
    const std::string bigBody(fl::kAdminChunkPayload, 'x');
    for (uint16_t i = 0; i < 130u; ++i) {
        auto pkt = makeChunkPacket(1u, i, 0u, bigBody);
        handler.onReceive(0u, pkt.data(), pkt.size());
    }

    CHECK(console.outputLines().empty());
}

TEST_CASE("ClientNetEventHandler: end chunk with empty body does not print",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    auto pkt = makeChunkPacket(1u, 0u, fl::kChunkFlagEnd, "");
    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(console.outputLines().empty());
}

TEST_CASE("ClientNetEventHandler: null console does not crash on chunk receipt",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    // handler.console left null (default)

    auto pkt = makeChunkPacket(1u, 0u, fl::kChunkFlagEnd, "hello");
    REQUIRE_NOTHROW(handler.onReceive(0u, pkt.data(), pkt.size()));
}

TEST_CASE("ClientNetEventHandler: second complete chunk stream prints correctly after first",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    // First complete stream.
    auto p0 = makeChunkPacket(1u, 0u, fl::kChunkFlagEnd, "first");
    handler.onReceive(0u, p0.data(), p0.size());

    // Second complete stream.
    auto p1 = makeChunkPacket(2u, 0u, fl::kChunkFlagEnd, "second");
    handler.onReceive(0u, p1.data(), p1.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 2u);
    CHECK(lines[0] == "[admin] first");
    CHECK(lines[1] == "[admin] second");
}

// ---------------------------------------------------------------------------
// Multi-line admin response splitting tests
// ---------------------------------------------------------------------------

TEST_CASE("ClientNetEventHandler: MsgAdminResponse multi-line body splits into one entry per line",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    fl::MsgAdminResponse resp{};
    std::snprintf(resp.text, sizeof(resp.text), "line1\nline2\nline3");
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&resp),
                                    reinterpret_cast<const uint8_t*>(&resp) + sizeof(resp));
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 3u);
    CHECK(lines[0] == "[admin] line1");
    CHECK(lines[1] == "[admin] line2");
    CHECK(lines[2] == "[admin] line3");
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponse empty lines in body are skipped",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    fl::MsgAdminResponse resp{};
    std::snprintf(resp.text, sizeof(resp.text), "section:\n\n  entry");
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&resp),
                                    reinterpret_cast<const uint8_t*>(&resp) + sizeof(resp));
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 2u);
    CHECK(lines[0] == "[admin] section:");
    CHECK(lines[1] == "[admin]   entry");
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponse trailing newline does not produce extra entry",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    fl::MsgAdminResponse resp{};
    std::snprintf(resp.text, sizeof(resp.text), "done\n");
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&resp),
                                    reinterpret_cast<const uint8_t*>(&resp) + sizeof(resp));
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 1u);
    CHECK(lines[0] == "[admin] done");
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponse CRLF line endings stripped",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    fl::MsgAdminResponse resp{};
    // Write CRLF body manually (snprintf would stop at \0, use memcpy).
    const char* body = "line1\r\nline2\r\n";
    std::memcpy(resp.text, body, std::strlen(body) + 1);
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&resp),
                                    reinterpret_cast<const uint8_t*>(&resp) + sizeof(resp));
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 2u);
    CHECK(lines[0] == "[admin] line1");
    CHECK(lines[1] == "[admin] line2");
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponse body of only newlines produces no output",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    fl::MsgAdminResponse resp{};
    const char* body = "\n\n\n";
    std::memcpy(resp.text, body, std::strlen(body) + 1);
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&resp),
                                    reinterpret_cast<const uint8_t*>(&resp) + sizeof(resp));
    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(console.outputLines().empty());
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponseChunk multi-line final chunk splits on print",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    auto pkt = makeChunkPacket(1u, 0u, fl::kChunkFlagEnd, "foo\nbar");
    handler.onReceive(0u, pkt.data(), pkt.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 2u);
    CHECK(lines[0] == "[admin] foo");
    CHECK(lines[1] == "[admin] bar");
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponseChunk newline spanning two chunks splits correctly",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    // First chunk carries the newline but no end flag — nothing printed yet.
    auto pkt0 = makeChunkPacket(1u, 0u, 0u, "line1\n");
    handler.onReceive(0u, pkt0.data(), pkt0.size());
    CHECK(console.outputLines().empty());

    // Second chunk ends the stream — assembled body "line1\nline2" splits into two entries.
    auto pkt1 = makeChunkPacket(1u, 1u, fl::kChunkFlagEnd, "line2");
    handler.onReceive(0u, pkt1.data(), pkt1.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 2u);
    CHECK(lines[0] == "[admin] line1");
    CHECK(lines[1] == "[admin] line2");
}

TEST_CASE("ClientNetEventHandler: MsgAdminResponseChunk consecutive multi-line streams reset correctly",
          "[client_net_event_handler][admin_chunk]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    CommandRegistry cmdRegistry;
    GameConsole console(logger, cmdRegistry);

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;

    // First complete multi-line stream.
    auto p0 = makeChunkPacket(1u, 0u, fl::kChunkFlagEnd, "a\nb");
    handler.onReceive(0u, p0.data(), p0.size());

    // Second complete multi-line stream — verifies m_chunkBuf.clear() reset correctly.
    auto p1 = makeChunkPacket(2u, 0u, fl::kChunkFlagEnd, "c\nd");
    handler.onReceive(0u, p1.data(), p1.size());

    auto lines = console.outputLines();
    REQUIRE(lines.size() == 4u);
    CHECK(lines[0] == "[admin] a");
    CHECK(lines[1] == "[admin] b");
    CHECK(lines[2] == "[admin] c");
    CHECK(lines[3] == "[admin] d");
}

TEST_CASE("ClientNetEventHandler: WorldSnapshot with SnapshotPeerCount extension stores peer count",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Build a WorldSnapshot with one full record followed by a SnapshotPeerCount TLV extension.
    TestRec rec;
    rec.idx = 1u;
    rec.gen = 1u;
    rec.isFull = true;
    auto pkt = buildSnapshotPkt(10u, {rec});
    const uint16_t kPeers = 5u;
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), kPeers);

    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(handler.serverPeerCount() == kPeers);
}

TEST_CASE("ClientNetEventHandler: WorldSnapshot without extension leaves peer count at zero",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Verify the default.
    CHECK(handler.serverPeerCount() == 0u);

    // Build a plain WorldSnapshot with no TLV extension.
    std::vector<uint8_t> pkt;
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.tickIndex = 1u;
    fl::appendMsg(pkt, hdr);

    handler.onReceive(0u, pkt.data(), pkt.size());

    // Peer count must remain at 0 — no extension was present.
    CHECK(handler.serverPeerCount() == 0u);
}

// ---------------------------------------------------------------------------
// Heartbeat / RTT (MsgPeerDelay) tests
// ---------------------------------------------------------------------------

// Helper: build a minimal WorldSnapshot packet with a given tickIndex.
static std::vector<uint8_t> makeSnapshotPacket(uint64_t tickIndex) {
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.tickIndex = tickIndex;
    std::vector<uint8_t> pkt;
    fl::appendMsg(pkt, hdr);
    return pkt;
}

// Helper: build a MsgPeerDelay packet.
static std::vector<uint8_t> makePeerDelayPacket(uint16_t delayTicks) {
    fl::MsgPeerDelay pd;
    pd.delayTicks = delayTicks;
    std::vector<uint8_t> pkt(sizeof(pd));
    std::memcpy(pkt.data(), &pd, sizeof(pd));
    return pkt;
}

TEST_CASE("ClientNetEventHandler: hasRtt false before first MsgPeerDelay", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    CHECK(!handler.hasRtt());
    CHECK(handler.lastRttMs() == 0u);
}

TEST_CASE("ClientNetEventHandler: MsgPeerDelay sets lastRttMs", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // 60 ticks * 1000 / 60 = 1000 ms
    auto pkt = makePeerDelayPacket(60u);
    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(handler.hasRtt());
    CHECK(handler.lastRttMs() == 1000u);
}

TEST_CASE("ClientNetEventHandler: MsgPeerDelay delayTicks zero does not set hasRtt", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    auto pkt = makePeerDelayPacket(0u);
    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(!handler.hasRtt());
}

TEST_CASE("ClientNetEventHandler: multiple MsgPeerDelay packets update to latest value", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    auto p30 = makePeerDelayPacket(30u);
    handler.onReceive(0u, p30.data(), p30.size()); // 30*1000/60 = 500 ms

    auto p60 = makePeerDelayPacket(60u);
    handler.onReceive(0u, p60.data(), p60.size()); // 60*1000/60 = 1000 ms

    CHECK(handler.lastRttMs() == 1000u);
}

TEST_CASE("ClientNetEventHandler: truncated MsgPeerDelay discarded", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    uint8_t tiny[1] = {static_cast<uint8_t>(fl::MsgId::PeerDelay)};
    handler.onReceive(0u, tiny, sizeof(tiny));

    CHECK(!handler.hasRtt());
}

TEST_CASE("ClientNetEventHandler: sendHeartbeatIfNeeded suppressed before first snapshot",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    fl::ManualClock clock;
    handler.setClock(clock);
    // Advance 2 seconds — but m_lastSnapshotTick is still 0
    clock.advance(std::chrono::seconds(2));

    handler.sendHeartbeatIfNeeded();

    CHECK(net.sends.empty()); // no heartbeat until a snapshot arrives
}

TEST_CASE("ClientNetEventHandler: sendHeartbeatIfNeeded sends on first call after snapshot",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    fl::ManualClock clock;
    handler.setClock(clock);

    // Receive a snapshot (sets m_lastSnapshotTick = 42)
    auto snap = makeSnapshotPacket(42u);
    handler.onReceive(0u, snap.data(), snap.size());

    // Advance past 1 second — first heartbeat should fire
    clock.advance(std::chrono::seconds(2));
    handler.sendHeartbeatIfNeeded();

    REQUIRE(net.sends.size() == 1u);
    REQUIRE(net.sends[0].size() == sizeof(fl::MsgHeartbeat));
    fl::MsgHeartbeat hb;
    std::memcpy(&hb, net.sends[0].data(), sizeof(hb));
    CHECK(hb.msgId == static_cast<uint8_t>(fl::MsgId::Heartbeat));
    CHECK(hb.tickIndex == 42u);
    CHECK(!net.sendReliable);
}

TEST_CASE("ClientNetEventHandler: sendHeartbeatIfNeeded throttles to 1 Hz", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    fl::ManualClock clock;
    handler.setClock(clock);

    auto snap = makeSnapshotPacket(1u);
    handler.onReceive(0u, snap.data(), snap.size());

    // First call fires immediately (epoch delta >> 1s)
    clock.advance(std::chrono::seconds(2));
    handler.sendHeartbeatIfNeeded();
    CHECK(net.sends.size() == 1u);

    // Second call within <1s: throttled
    clock.advance(std::chrono::milliseconds(500));
    handler.sendHeartbeatIfNeeded();
    CHECK(net.sends.size() == 1u);

    // Third call after 1s total: fires
    clock.advance(std::chrono::milliseconds(600));
    handler.sendHeartbeatIfNeeded();
    CHECK(net.sends.size() == 2u);
}

TEST_CASE("ClientNetEventHandler: sendHeartbeatIfNeeded uses last received snapshot tickIndex",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    fl::ManualClock clock;
    handler.setClock(clock);

    // Two snapshots: tick 5 then tick 42
    auto snap5 = makeSnapshotPacket(5u);
    handler.onReceive(0u, snap5.data(), snap5.size());
    auto snap42 = makeSnapshotPacket(42u);
    handler.onReceive(0u, snap42.data(), snap42.size());

    clock.advance(std::chrono::seconds(2));
    handler.sendHeartbeatIfNeeded();

    REQUIRE(net.sends.size() == 1u);
    fl::MsgHeartbeat hb;
    std::memcpy(&hb, net.sends[0].data(), sizeof(hb));
    CHECK(hb.tickIndex == 42u); // most recent snapshot tick
}

// ---------------------------------------------------------------------------
// MsgEntityUpdate (delta compression) parsing tests (#346)
// ---------------------------------------------------------------------------

static std::vector<uint8_t> makeFullThenUpdatePacket(uint32_t entityIdx, uint32_t entityGen, uint32_t typeIndex,
                                                     bool sendUpdate) {
    std::vector<TestRec> recs;
    TestRec full;
    full.idx = entityIdx;
    full.gen = entityGen;
    full.typeIndex = typeIndex;
    full.isFull = true;
    full.pos[1] = 500.0;
    recs.push_back(full);
    if (sendUpdate) {
        // Note: records must be sorted by idx ascending; this delta shares entityIdx so it would
        // collide — kept distinct in callers. Here the update reuses the same idx for a single-entity
        // packet, which is not a valid multi-record stream; callers that need both use buildSnapshotPkt.
        TestRec upd;
        upd.idx = entityIdx;
        upd.gen = entityGen;
        upd.pos[0] = 1000.f;
        upd.pos[1] = 550.f;
        recs.push_back(upd);
    }
    return buildSnapshotPkt(1u, recs);
}

TEST_CASE("ClientNetEventHandler: MsgEntityUpdate decoded using cached typeIndex", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // First snapshot: full entry for entity 5, typeIndex=7
    auto pkt1 = makeFullThenUpdatePacket(5u, 1u, 7u, /*sendUpdate=*/false);
    handler.onReceive(0u, pkt1.data(), pkt1.size());
    bridge.tryAdvance();
    REQUIRE(bridge.current().entries.size() == 1u);
    CHECK(bridge.current().entries[0].typeIndex == 7u);

    // Second snapshot: delta record only (gen omitted → client uses cache).
    {
        TestRec upd;
        upd.idx = 5u;
        upd.isFull = false;
        upd.pos[0] = 999.0;
        upd.pos[1] = 600.0;
        upd.throttle = 80u;
        auto pkt2 = buildSnapshotPkt(2u, {upd});

        handler.onReceive(0u, pkt2.data(), pkt2.size());
        bridge.tryAdvance();
        REQUIRE(bridge.current().entries.size() == 1u);
        CHECK(bridge.current().entries[0].typeIndex == 7u); // from cache
        CHECK(Catch::Approx(bridge.current().entries[0].position.x).margin(0.2) == 999.0);
        CHECK(bridge.current().entries[0].throttle == 80u);
    }
}

TEST_CASE("ClientNetEventHandler: MsgEntityUpdate skipped when entity not in cache", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Send delta-only snapshot for an entity not previously seen.
    TestRec upd;
    upd.idx = 42u;
    upd.isFull = false;
    auto pkt = buildSnapshotPkt(1u, {upd});

    handler.onReceive(0u, pkt.data(), pkt.size());
    bridge.tryAdvance();
    // Unknown entity — gracefully skipped
    CHECK(bridge.current().entries.empty());
}

TEST_CASE("ClientNetEventHandler: MsgEntityUpdate skipped when entityGen mismatches cache",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Cache entity 3 with gen=1
    {
        TestRec full;
        full.idx = 3u;
        full.gen = 1u;
        full.typeIndex = 5u;
        full.isFull = true;
        auto pkt = buildSnapshotPkt(1u, {full});
        handler.onReceive(0u, pkt.data(), pkt.size());
    }
    bridge.tryAdvance();

    // Send delta carrying gen=2 (genPresent, mismatches cached gen=1) → should be skipped
    {
        TestRec upd;
        upd.idx = 3u;
        upd.gen = 2u;
        upd.isFull = false;
        upd.sendGen = true; // force the genPresent bit so the stale-gen guard can fire
        auto pkt = buildSnapshotPkt(2u, {upd});
        handler.onReceive(0u, pkt.data(), pkt.size());
    }
    bridge.tryAdvance();
    // The stale-gen delta is ignored; under entity retention (#516) the previously-cached gen=1
    // entity persists with its last-known state rather than vanishing.
    REQUIRE(bridge.current().entries.size() == 1u);
    CHECK(bridge.current().entries[0].entityIdx == 3u);
    CHECK(bridge.current().entries[0].entityGen == 1u);
}

TEST_CASE("ClientNetEventHandler: SnapshotPeerCount TLV at correct offset after full+update records",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Cache entity 10 first (full record).
    {
        TestRec full;
        full.idx = 10u;
        full.gen = 1u;
        full.typeIndex = 2u;
        full.isFull = true;
        auto pkt0 = buildSnapshotPkt(1u, {full});
        handler.onReceive(0u, pkt0.data(), pkt0.size());
    }
    bridge.tryAdvance();

    // Now send a delta for entity 10 + a full record for entity 11 (sorted by idx) + TLV.
    TestRec d10;
    d10.idx = 10u;
    d10.isFull = false;
    d10.pos[0] = 50.0;
    TestRec f11;
    f11.idx = 11u;
    f11.gen = 1u;
    f11.typeIndex = 3u;
    f11.isFull = true;
    auto pkt = buildSnapshotPkt(2u, {d10, f11});
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), uint16_t{7u});

    handler.onReceive(0u, pkt.data(), pkt.size());
    bridge.tryAdvance();

    // Both entries should be parsed
    CHECK(bridge.current().entries.size() == 2u);
    // TLV should be parsed correctly
    CHECK(handler.serverPeerCount() == 7u);
}

// ---------------------------------------------------------------------------
// SnapshotPeerLatency TLV parsing tests (#382)
// ---------------------------------------------------------------------------

TEST_CASE("ClientNetEventHandler: WorldSnapshot with SnapshotPeerLatency TLV stores latency",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    CHECK(!handler.hasSnapshotLatency());
    CHECK(handler.snapshotLatencyMs() == 0u);

    auto pkt = buildSnapshotPkt(1u, {});
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), uint16_t{120u});

    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(handler.hasSnapshotLatency());
    CHECK(handler.snapshotLatencyMs() == 120u);
}

TEST_CASE("ClientNetEventHandler: WorldSnapshot without SnapshotPeerLatency leaves state at zero",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    auto pkt = buildSnapshotPkt(1u, {});

    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(!handler.hasSnapshotLatency());
    CHECK(handler.snapshotLatencyMs() == 0u);
}

TEST_CASE("ClientNetEventHandler: SnapshotPeerLatency TLV parsed correctly alongside SnapshotPeerCount",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    TestRec rec;
    rec.idx = 3u;
    rec.gen = 1u;
    rec.isFull = true;
    auto pkt = buildSnapshotPkt(5u, {rec});
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), uint16_t{4u});
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), uint16_t{83u});

    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(handler.serverPeerCount() == 4u);
    CHECK(handler.hasSnapshotLatency());
    CHECK(handler.snapshotLatencyMs() == 83u);
}

TEST_CASE("ClientNetEventHandler: MsgEntityUpdate omega parsed into EntityRenderEntry", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // First, send a full record so the entity is in m_knownEntities.
    {
        TestRec full;
        full.idx = 10u;
        full.gen = 1u;
        full.isFull = true;
        auto pkt = buildSnapshotPkt(1u, {full});
        handler.onReceive(0u, pkt.data(), pkt.size());
    }

    // Now send a delta for the same entity carrying omega (hasOmega = own-entity record).
    {
        TestRec upd;
        upd.idx = 10u;
        upd.isFull = false;
        upd.hasOmega = true;
        upd.omega[0] = 1.1f;
        upd.omega[1] = 2.2f;
        upd.omega[2] = 3.3f;
        auto pkt = buildSnapshotPkt(2u, {upd});
        handler.onReceive(0u, pkt.data(), pkt.size());
    }

    bridge.tryAdvance();
    const auto& snap = bridge.current();
    const auto* entry = [&]() -> const fl::EntityRenderEntry* {
        for (const auto& e : snap.entries) {
            if (e.entityIdx == 10u) {
                return &e;
            }
        }
        return nullptr;
    }();
    REQUIRE(entry != nullptr);
    CHECK(entry->omega.x == Catch::Approx(1.1f).margin(0.02));
    CHECK(entry->omega.y == Catch::Approx(2.2f).margin(0.02));
    CHECK(entry->omega.z == Catch::Approx(3.3f).margin(0.02));
}

TEST_CASE("ClientNetEventHandler: SnapshotPeerDelayTicks TLV updates m_estimatedDelayTicks",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    std::vector<uint8_t> pkt;
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.tickIndex = 10u;
    fl::appendMsg(pkt, hdr);
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), uint16_t{1u});
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), uint16_t{83u});
    fl::appendExt(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerDelayTicks), uint16_t{5u});

    uint32_t capturedDelayTicks{0};
    handler.snapshotCallback = [&](fl::RenderSnapshot&, uint64_t, uint32_t delayTicks, uint32_t) {
        capturedDelayTicks = delayTicks;
    };

    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(capturedDelayTicks == 5u);
}

TEST_CASE("ClientNetEventHandler: snapshotCallback called before publishExternal", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    int callbackOrder{0};
    int callbackFired{0};
    handler.snapshotCallback = [&](fl::RenderSnapshot&, uint64_t, uint32_t, uint32_t) {
        // At callback time, bridge should NOT yet have the snapshot (publishExternal not called).
        callbackFired = ++callbackOrder;
        CHECK(!bridge.hasSnapshot());
    };

    std::vector<uint8_t> pkt;
    fl::MsgWorldSnapshotHeader hdr{};
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.tickIndex = 1u;
    fl::appendMsg(pkt, hdr);
    handler.onReceive(0u, pkt.data(), pkt.size());

    CHECK(callbackFired == 1);
    bridge.tryAdvance();
    CHECK(bridge.hasSnapshot()); // published after callback
}

// ---------------------------------------------------------------------------
// Entity retention + despawn (priority/budget scheduler, #516)
// ---------------------------------------------------------------------------

// Append a SnapshotDespawn TLV (uint32[] of indices) to a snapshot packet built by buildSnapshotPkt.
static void appendDespawnTlv(std::vector<uint8_t>& pkt, const std::vector<uint32_t>& ids) {
    fl::appendExtRaw(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn), ids.data(),
                     static_cast<uint16_t>(ids.size() * sizeof(uint32_t)));
}

static bool entriesContain(const fl::RenderSnapshot& snap, uint32_t idx) {
    for (const auto& e : snap.entries)
        if (e.entityIdx == idx)
            return true;
    return false;
}

TEST_CASE("ClientNetEventHandler: budget-deferred entity is retained across snapshots",
          "[client_net_event_handler][retention]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Tick 1: learn entities 1 and 2 (full records).
    TestRec a, b;
    a.idx = 1u;
    a.gen = 1u;
    a.typeIndex = 5u;
    a.isFull = true;
    b.idx = 2u;
    b.gen = 1u;
    b.typeIndex = 5u;
    b.isFull = true;
    auto pkt1 = buildSnapshotPkt(1u, {a, b});
    handler.onReceive(0u, pkt1.data(), pkt1.size());

    // Tick 2: only entity 1 is sent (entity 2 is budget-deferred this tick).
    TestRec aDelta;
    aDelta.idx = 1u;
    aDelta.gen = 1u;
    aDelta.isFull = false;
    auto pkt2 = buildSnapshotPkt(2u, {aDelta});
    handler.onReceive(0u, pkt2.data(), pkt2.size());

    bridge.tryAdvance();
    // Entity 2 is retained even though it was absent from the second snapshot (no flicker).
    CHECK(entriesContain(bridge.current(), 1u));
    CHECK(entriesContain(bridge.current(), 2u));
}

TEST_CASE("ClientNetEventHandler: SnapshotDespawn TLV removes the entity from the render set",
          "[client_net_event_handler][retention]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    TestRec a, b;
    a.idx = 1u;
    a.gen = 1u;
    a.typeIndex = 5u;
    a.isFull = true;
    b.idx = 2u;
    b.gen = 1u;
    b.typeIndex = 5u;
    b.isFull = true;
    auto pkt1 = buildSnapshotPkt(1u, {a, b});
    handler.onReceive(0u, pkt1.data(), pkt1.size());

    // Tick 2: entity 1 only, plus an explicit despawn of entity 2.
    TestRec aDelta;
    aDelta.idx = 1u;
    aDelta.gen = 1u;
    aDelta.isFull = false;
    auto pkt2 = buildSnapshotPkt(2u, {aDelta});
    appendDespawnTlv(pkt2, {2u});
    handler.onReceive(0u, pkt2.data(), pkt2.size());

    bridge.tryAdvance();
    CHECK(entriesContain(bridge.current(), 1u));
    CHECK_FALSE(entriesContain(bridge.current(), 2u)); // explicitly despawned
}

TEST_CASE("ClientNetEventHandler: despawn applied before upsert so same-idx respawn survives",
          "[client_net_event_handler][retention]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Tick 1: entity 3 at generation 1.
    TestRec gen1;
    gen1.idx = 3u;
    gen1.gen = 1u;
    gen1.typeIndex = 5u;
    gen1.isFull = true;
    auto pkt1 = buildSnapshotPkt(1u, {gen1});
    handler.onReceive(0u, pkt1.data(), pkt1.size());

    // Tick 2: slot 3 reused — despawn of old gen + full record of the new gen 2 in ONE packet. The
    // despawn must be applied before the record upsert so the new entity is not deleted.
    TestRec gen2;
    gen2.idx = 3u;
    gen2.gen = 2u;
    gen2.typeIndex = 5u;
    gen2.isFull = true;
    auto pkt2 = buildSnapshotPkt(2u, {gen2});
    appendDespawnTlv(pkt2, {3u});
    handler.onReceive(0u, pkt2.data(), pkt2.size());

    bridge.tryAdvance();
    REQUIRE(entriesContain(bridge.current(), 3u));
    for (const auto& e : bridge.current().entries)
        if (e.entityIdx == 3u)
            CHECK(e.entityGen == 2u); // the respawned entity, not the deleted one
}

TEST_CASE("ClientNetEventHandler: retained entity ages out after the retention window",
          "[client_net_event_handler][retention]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Tick 1: learn entities 1 and 2.
    TestRec a, b;
    a.idx = 1u;
    a.gen = 1u;
    a.typeIndex = 5u;
    a.isFull = true;
    b.idx = 2u;
    b.gen = 1u;
    b.typeIndex = 5u;
    b.isFull = true;
    auto pkt1 = buildSnapshotPkt(1u, {a, b});
    handler.onReceive(0u, pkt1.data(), pkt1.size());

    // Keep sending only entity 1 well past the retention window; entity 2 must eventually age out.
    for (uint64_t tick = 2; tick <= fl::kSnapshotRetentionTicks + 5u; ++tick) {
        TestRec aDelta;
        aDelta.idx = 1u;
        aDelta.gen = 1u;
        aDelta.isFull = false;
        auto pkt = buildSnapshotPkt(tick, {aDelta});
        handler.onReceive(0u, pkt.data(), pkt.size());
    }

    bridge.tryAdvance();
    CHECK(entriesContain(bridge.current(), 1u));
    CHECK_FALSE(entriesContain(bridge.current(), 2u)); // evicted by the retention timeout
}

// -- Selective-ack mask maintenance (#566) -----------------------------------------------------------

namespace {
// Feed a one-record full snapshot at `tick` so the handler accepts and decodes it.
void feedTick(ClientNetEventHandler& handler, uint64_t tick) {
    TestRec a;
    a.idx = 1u;
    a.gen = 1u;
    a.isFull = true;
    a.pos[1] = 500.0;
    auto pkt = buildSnapshotPkt(tick, {a});
    handler.onReceive(0u, pkt.data(), pkt.size());
}
} // namespace

TEST_CASE("ClientNetEventHandler: consecutive snapshots fill the selective-ack mask",
          "[client_net_event_handler][identity-ack]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    for (uint64_t tick = 1; tick <= 4; ++tick)
        feedTick(handler, tick);

    // stampAck reports the high-water tick + the decoded-tick bitmask. Ticks 1-3 (below high-water 4)
    // were all decoded → bits 0..2 set.
    MsgClientInput inp{};
    handler.stampAck(inp);
    CHECK(inp.tickIndex == 4u);
    CHECK(inp.ackMask == 0b111u);

    // The heartbeat path stamps the same pair.
    MsgHeartbeat hb{};
    handler.stampAck(hb);
    CHECK(hb.tickIndex == 4u);
    CHECK(hb.ackMask == 0b111u);
}

TEST_CASE("ClientNetEventHandler: a dropped snapshot leaves its ack bit clear",
          "[client_net_event_handler][identity-ack]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    feedTick(handler, 1u);
    feedTick(handler, 2u);
    // Tick 3 is dropped (never delivered); tick 4 arrives with a gap.
    feedTick(handler, 4u);

    MsgClientInput inp{};
    handler.stampAck(inp);
    CHECK(inp.tickIndex == 4u);
    // High-water 4: bit 0 = tick 3 (dropped → clear), bit 1 = tick 2, bit 2 = tick 1.
    CHECK(inp.ackMask == 0b110u);
}

TEST_CASE("ClientNetEventHandler: a reordered/duplicate snapshot does not set its ack bit",
          "[client_net_event_handler][identity-ack]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    feedTick(handler, 4u);
    feedTick(handler, 5u);
    // A stale (already-superseded) tick 3 arrives late; the handler drops it WITHOUT decoding, so it
    // must NOT be marked as decoded — proving the deliberate "no ackMark" decision.
    feedTick(handler, 3u);

    MsgClientInput inp{};
    handler.stampAck(inp);
    CHECK(inp.tickIndex == 5u); // high-water unchanged by the stale packet
    CHECK(inp.ackMask == 0b1u); // only tick 4 (bit 0) decoded; tick 3's bit (age 2) stays clear
}

TEST_CASE("ClientNetEventHandler: a jump beyond the ack window resets the mask",
          "[client_net_event_handler][identity-ack]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    feedTick(handler, 10u);
    feedTick(handler, 50u); // delta 40 > 32-tick window → the whole mask rolls out

    MsgClientInput inp{};
    handler.stampAck(inp);
    CHECK(inp.tickIndex == 50u);
    CHECK(inp.ackMask == 0u);
}

// Regression (#94 fuzzing): a MsgConnectAck whose trailing MsgEntityTypeDef char[] fields are fully
// populated (NOT NUL-terminated) must not over-read when copied into std::string / passed to findById.
TEST_CASE("ClientNetEventHandler: non-terminated type-def fields do not over-read", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    std::vector<uint8_t> pkt;
    fl::MsgConnectAck ack{};
    ack.typeCount = 1;
    fl::appendMsg(pkt, ack);

    fl::MsgEntityTypeDef td{};
    td.typeIndex = 1;
    std::memset(td.id, 'A', sizeof(td.id)); // fully filled: no NUL terminator
    std::memset(td.mesh, 'B', sizeof(td.mesh));
    std::memset(td.dmgMesh, 'C', sizeof(td.dmgMesh));
    fl::appendMsg(pkt, td);

    // Must not over-read past the char[] fields (ASan/UBSan would catch); the type registers with a
    // truncated, in-bounds id.
    handler.onReceive(0u, pkt.data(), pkt.size());
    CHECK(registry.typeCount() >= 1u);
}

// #886: MsgEntityTypeDef carries the ObjectCategory + ProjectileKind ordinals, so the client-side
// registry can select per-category placeholder silhouettes. Invalid wire ordinals (a hostile or
// newer server) are gated before the enum cast and fall back to the pre-#886 defaults.
TEST_CASE("ClientNetEventHandler: type-def category and projectileKind parse with ordinal gating",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    std::vector<uint8_t> pkt;
    fl::MsgConnectAck ack{};
    ack.typeCount = 3;
    fl::appendMsg(pkt, ack);

    fl::MsgEntityTypeDef structureTd{};
    structureTd.typeIndex = 0;
    std::snprintf(structureTd.id, sizeof(structureTd.id), "builtin:static-target");
    structureTd.category = static_cast<uint8_t>(fl::ObjectCategory::Structure);
    fl::appendMsg(pkt, structureTd);

    fl::MsgEntityTypeDef bombTd{};
    bombTd.typeIndex = 1;
    std::snprintf(bombTd.id, sizeof(bombTd.id), "projectile:builtin:bomb");
    bombTd.category = static_cast<uint8_t>(fl::ObjectCategory::Projectile);
    bombTd.projectileKind = static_cast<uint8_t>(fl::ProjectileKind::Bomb);
    fl::appendMsg(pkt, bombTd);

    fl::MsgEntityTypeDef hostileTd{}; // out-of-range ordinals from an untrusted server
    hostileTd.typeIndex = 2;
    std::snprintf(hostileTd.id, sizeof(hostileTd.id), "evil:type");
    hostileTd.category = 0xEE;
    hostileTd.projectileKind = 0xEE;
    fl::appendMsg(pkt, hostileTd);

    handler.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(registry.typeCount() == 3u);
    CHECK(registry.findById("builtin:static-target")->category == fl::ObjectCategory::Structure);
    CHECK(registry.findById("builtin:static-target")->projectileKind == fl::ProjectileKind::None);
    CHECK(registry.findById("projectile:builtin:bomb")->category == fl::ObjectCategory::Projectile);
    CHECK(registry.findById("projectile:builtin:bomb")->projectileKind == fl::ProjectileKind::Bomb);
    // Gated: invalid bytes fall back to the defaults rather than casting out-of-range enums.
    CHECK(registry.findById("evil:type")->category == fl::ObjectCategory::AirVehicle);
    CHECK(registry.findById("evil:type")->projectileKind == fl::ProjectileKind::None);
}

TEST_CASE("ClientNetEventHandler: CombatEvent feeds the kill feed and the session stats",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    fl::CommandRegistry cmdReg;
    fl::GameConsole console(logger, cmdReg);
    ServerNotice notice;

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.console = &console;
    handler.notice = &notice;
    handler.assignedEntityIdx = 5;
    handler.assignedEntityGen = 2;

    std::vector<uint8_t> pkt;
    fl::MsgCombatEventHeader hdr;
    hdr.count = 2;
    fl::appendMsg(pkt, hdr);
    fl::CombatEventRecord kill{};
    kill.type = static_cast<uint8_t>(fl::CombatEventType::Kill);
    kill.subjectIdx = 9;
    kill.subjectGen = 1;
    kill.instigatorIdx = 5; // our own aircraft: this is OUR kill
    kill.instigatorGen = 2;
    kill.a = 0; // we are peer 0
    kill.b = fl::kNoOwningPeer;
    fl::appendMsg(pkt, kill);
    fl::CombatEventRecord stats{};
    stats.type = static_cast<uint8_t>(fl::CombatEventType::Stats);
    stats.a = 3;
    stats.b = 1;
    stats.c = 3;
    fl::appendMsg(pkt, stats);

    handler.onReceive(0u, pkt.data(), pkt.size());

    // Kill feed reached the console...
    bool sawKillLine = false;
    for (const auto& line : console.outputLines())
        if (line.find("[kill] you destroyed") != std::string::npos)
            sawKillLine = true;
    CHECK(sawKillLine);
    // ...the banner celebrates the right party...
    auto elems = notice.buildElements();
    REQUIRE(!elems.empty());
    CHECK(std::string(elems[0].text).find("DESTROYED") != std::string::npos);
    // ...and the tallies are the SERVER's, verbatim.
    CHECK(handler.sessionStats().kills == 3u);
    CHECK(handler.sessionStats().losses == 1u);
    CHECK(handler.sessionStats().score == 3);
}

TEST_CASE("ClientNetEventHandler: a truncated CombatEvent packet fails closed", "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    std::vector<uint8_t> pkt;
    fl::MsgCombatEventHeader hdr;
    hdr.count = 4; // claims four records, carries half of one
    fl::appendMsg(pkt, hdr);
    pkt.resize(pkt.size() + sizeof(fl::CombatEventRecord) / 2, 0u);

    handler.onReceive(0u, pkt.data(), pkt.size()); // must not read past the buffer or crash
    CHECK(handler.sessionStats().kills == 0u);
}

// ── datalink track picture (#528) ────────────────────────────────────────────

TEST_CASE("ClientNetEventHandler: MsgDatalink round-trips tracks and RWR to absolute positions",
          "[client_net_event_handler][datalink]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // Before any datalink, the view is invalid — the HUD draws nothing rather than an empty scope.
    CHECK_FALSE(handler.radarView().valid);

    // Build a server-side MsgDatalink: one foe track (own-sensor, firing-quality) + one lock strobe,
    // positions RELATIVE to an origin far from the world origin (proves the reconstruction).
    std::vector<uint8_t> buf;
    fl::MsgDatalinkHeader hdr;
    hdr.trackCount = 1;
    hdr.threatCount = 1;
    hdr.tickIndex = 1234;
    hdr.origin[0] = 1'000'000.0;
    hdr.origin[1] = 500.0;
    hdr.origin[2] = -2'000'000.0;
    fl::appendMsg(buf, hdr);

    fl::DatalinkTrack tr{};
    tr.targetIdx = 42;
    tr.targetGen = 3;
    tr.typeIndex = 7;
    tr.factionIndex = 2;
    tr.state = fl::kTrackLocked;
    tr.ident = fl::kIffFoe;
    tr.sensorTypeMask = 0x4;
    tr.flags = fl::kDatalinkFlagFiringQuality | fl::kDatalinkFlagOwnSensor;
    tr.relPos[0] = 3000.f;
    tr.relPos[1] = 10.f;
    tr.relPos[2] = -500.f;
    tr.relVel[0] = 250.f;
    fl::appendMsg(buf, tr);

    fl::DatalinkThreat th{};
    th.emitterIdx = 99;
    th.emitterGen = 1;
    th.channel = 2; // radar
    th.level = fl::kThreatLock;
    th.ident = fl::kIffFoe;
    th.relPos[0] = -1000.f;
    fl::appendMsg(buf, th);

    handler.onReceive(0u, buf.data(), buf.size());

    const fl::RadarView v = handler.radarView();
    REQUIRE(v.valid);
    REQUIRE(v.tracks.size() == 1u);
    const fl::RadarTrack& t = v.tracks[0];
    CHECK(t.entityIdx == 42u);
    CHECK(t.ident == fl::kIffFoe);
    CHECK(t.firingQuality);
    CHECK(t.ownSensor);
    // Absolute position = header origin + relative payload.
    CHECK(t.pos[0] == Catch::Approx(1'003'000.0));
    CHECK(t.pos[2] == Catch::Approx(-2'000'500.0));

    REQUIRE(v.strobes.size() == 1u);
    CHECK(v.strobes[0].emitterIdx == 99u);
    CHECK(v.strobes[0].level == fl::kThreatLock);
    CHECK(v.strobes[0].emitterPos[0] == Catch::Approx(999'000.0));
}

TEST_CASE("ClientNetEventHandler: a truncated MsgDatalink leaves the prior picture intact",
          "[client_net_event_handler][datalink]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // A header claiming two tracks but with no record bytes: must be rejected, not partially applied.
    std::vector<uint8_t> buf;
    fl::MsgDatalinkHeader hdr;
    hdr.trackCount = 2;
    hdr.threatCount = 0;
    fl::appendMsg(buf, hdr);
    handler.onReceive(0u, buf.data(), buf.size());
    CHECK_FALSE(handler.radarView().valid); // short packet ignored
}

TEST_CASE("ClientNetEventHandler: MsgCrewRoster stored and queryable by entity (#972)",
          "[client_net_event_handler][crew]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    std::vector<uint8_t> buf;
    fl::MsgCrewRosterHeader hdr{};
    hdr.seatCount = 2;
    hdr.turretCount = 1;
    hdr.entityIdx = 12;
    hdr.entityGen = 4;
    fl::appendMsg(buf, hdr);
    fl::CrewRosterSeat s0{};
    s0.seatIndex = 0;
    s0.occupancy = static_cast<uint8_t>(fl::SeatOccupancy::Human);
    s0.capabilities = 0x03;
    s0.occupantPeerId = 2;
    s0.turretIndex = 255;
    std::snprintf(s0.role, sizeof(s0.role), "pilot");
    fl::appendMsg(buf, s0);
    fl::CrewRosterSeat s1{};
    s1.seatIndex = 1;
    s1.occupancy = static_cast<uint8_t>(fl::SeatOccupancy::Bot);
    s1.capabilities = 0x02;
    s1.turretIndex = 0;
    s1.knockedOut = 1; // #978
    std::snprintf(s1.role, sizeof(s1.role), "tail-gunner");
    fl::appendMsg(buf, s1);

    handler.onReceive(0u, buf.data(), buf.size());

    const fl::CrewRosterInfo* roster = handler.crewRoster(12u);
    REQUIRE(roster != nullptr);
    CHECK(roster->entityGen == 4u);
    CHECK(roster->turretCount == 1);
    REQUIRE(roster->seats.size() == 2u);
    CHECK(roster->seats[0].occupancy == static_cast<uint8_t>(fl::SeatOccupancy::Human));
    CHECK(roster->seats[0].occupantPeerId == 2u);
    CHECK(roster->seats[0].role == "pilot");
    CHECK(roster->seats[1].occupancy == static_cast<uint8_t>(fl::SeatOccupancy::Bot));
    CHECK(roster->seats[1].turretIndex == 0);
    CHECK(roster->seats[1].knockedOut); // #978
    CHECK_FALSE(roster->seats[0].knockedOut);

    // An unknown entity has no roster.
    CHECK(handler.crewRoster(999u) == nullptr);

    // A short packet does not replace the stored roster.
    handler.onReceive(0u, buf.data(), sizeof(hdr) + 3u);
    REQUIRE(handler.crewRoster(12u) != nullptr);
    CHECK(handler.crewRoster(12u)->seats.size() == 2u);
}

TEST_CASE("ClientNetEventHandler: SnapshotCrew TLV decodes live turret pose (#972)",
          "[client_net_event_handler][crew]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    TestRec rec;
    rec.idx = 7;
    rec.gen = 1;
    rec.isFull = true;
    auto pkt = buildSnapshotPkt(1u, {rec});

    // One crewed entity (idx 7) with one turret: az = +pi/2 (looking left), el = +pi/4.
    constexpr float kPi = 3.14159265358979323846f;
    const auto azQ = static_cast<int16_t>(std::lround(0.5f * 32767.f)); // +pi/2 over [-pi,pi]
    const auto elQ = static_cast<int16_t>(std::lround(0.5f * 32767.f)); // +pi/4 over [-pi/2,pi/2]
    std::vector<uint8_t> payload;
    auto pushBytes = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        payload.insert(payload.end(), b, b + n);
    };
    payload.push_back(1u); // entryCount
    const uint32_t idx = 7;
    pushBytes(&idx, 4);
    payload.push_back(1u); // turretCount
    pushBytes(&azQ, 2);
    pushBytes(&elQ, 2);
    fl::appendExtRaw(pkt, static_cast<uint16_t>(fl::ExtTag::SnapshotCrew), payload.data(),
                     static_cast<uint16_t>(payload.size()));

    handler.onReceive(0u, pkt.data(), pkt.size());

    const auto poses = handler.crewTurretPoses(7u);
    REQUIRE(poses.size() == 1u);
    CHECK(poses[0].azRad == Catch::Approx(kPi * 0.5f).margin(0.01));
    CHECK(poses[0].elRad == Catch::Approx(kPi * 0.25f).margin(0.01));

    // An entity with no crew TLV entry has no poses.
    CHECK(handler.crewTurretPoses(999u).empty());
}

TEST_CASE("ClientNetEventHandler: seat request/result plumbing (#975)", "[client_net_event_handler][crew]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler(bridge, registry, logger, net, env);

    // sendSeatRequest emits a MsgSeatRequest join.
    handler.sendSeatRequest(42u, 3u, /*seat=*/1);
    REQUIRE(!net.sends.empty());
    fl::MsgSeatRequest req{};
    std::memcpy(&req, net.sends.back().data(), sizeof(req));
    CHECK(req.msgId == static_cast<uint8_t>(fl::MsgId::SeatRequest));
    CHECK(req.entityIdx == 42u);
    CHECK(req.seatIndex == 1);
    CHECK((req.flags & fl::kSeatRequestFlagLeave) == 0u);

    // sendSeatLeave sets the leave flag.
    handler.sendSeatLeave();
    std::memcpy(&req, net.sends.back().data(), sizeof(req));
    CHECK((req.flags & fl::kSeatRequestFlagLeave) != 0u);

    // A granted JOIN result marks the client as in a (non-fly) crew seat and is surfaced once.
    CHECK_FALSE(handler.inCrewSeat());
    fl::MsgSeatResult res{};
    res.code = static_cast<uint8_t>(fl::SeatResultCode::Granted);
    res.entityIdx = 42u;
    res.seatIndex = 1;
    handler.onReceive(0u, &res, sizeof(res));
    CHECK(handler.inCrewSeat());
    auto view = handler.takeSeatResult();
    CHECK(view.valid);
    CHECK(view.fresh);
    CHECK(view.code == static_cast<uint8_t>(fl::SeatResultCode::Granted));
    CHECK(handler.takeSeatResult().fresh == false); // consumed

    // A granted LEAVE (entityIdx 0) clears the crew-seat flag.
    fl::MsgSeatResult leave{};
    leave.code = static_cast<uint8_t>(fl::SeatResultCode::Granted);
    leave.entityIdx = 0u;
    handler.onReceive(0u, &leave, sizeof(leave));
    CHECK_FALSE(handler.inCrewSeat());

    // A denial does not change the crew-seat flag.
    fl::MsgSeatResult deny{};
    deny.code = static_cast<uint8_t>(fl::SeatResultCode::SeatOccupiedByHuman);
    deny.entityIdx = 42u;
    handler.onReceive(0u, &deny, sizeof(deny));
    CHECK_FALSE(handler.inCrewSeat());
    CHECK(handler.lastSeatResult().code == static_cast<uint8_t>(fl::SeatResultCode::SeatOccupiedByHuman));
}
