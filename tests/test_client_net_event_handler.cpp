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
#include "net/GameProtocol.h"
#include "net/WireCodec.h"

#include "SessionStatus.h"
#include "render/SimRenderBridge.h"

#include "mock_network.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

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

    // Build a raw WorldSnapshot packet with one entity entry carrying new telemetry fields.
    std::vector<uint8_t> pkt(sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry), 0);

    fl::MsgWorldSnapshotHeader hdr{};
    hdr.msgId = static_cast<uint8_t>(fl::MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(fl::kProtocolVersion);
    hdr.entityCount = 1u;
    hdr.tickIndex = 1u;
    std::memcpy(pkt.data(), &hdr, sizeof(hdr));

    fl::MsgEntityEntry entry{};
    entry.entityIdx = 0u;
    entry.entityGen = 1u;
    entry.abEngaged = 1u;
    entry.engineFailFlags = fl::kEngineFailGeneric;
    std::memcpy(pkt.data() + sizeof(hdr), &entry, sizeof(entry));

    handler.onReceive(0u, pkt.data(), pkt.size());

    bridge.tryAdvance();
    const auto& snap = bridge.current();
    REQUIRE(snap.entries.size() == 1u);
    CHECK(snap.entries[0].abEngaged == true);
    CHECK(snap.entries[0].engineFailFlags == fl::kEngineFailGeneric);
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

TEST_CASE("ClientNetEventHandler: onDisconnect does not signal when assignedEntityIdx is nonzero",
          "[client_net_event_handler]") {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    MockLogger logger;
    MockNetwork net;
    EnvironmentState env{};
    std::atomic<SessionFailure> failMsg{SessionFailure::None};

    ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.sessionFailure = &failMsg;
    handler.assignedEntityIdx = 1u; // simulates mid-flight disconnect

    handler.onConnect(0u);
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
    // planetRadiusKm stays at its default 0
    CHECK(handler.planetRadiusKm() == 0.0f);
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
