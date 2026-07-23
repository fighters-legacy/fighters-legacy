// SPDX-License-Identifier: GPL-3.0-or-later
#include "GmMapOverlay.h"

#include "ClientNetEventHandler.h"

#include "entity/EntityTypeRegistry.h"
#include "mock_gui.h"
#include "mock_hal.h"
#include "mock_network.h"
#include "net/GameProtocol.h"
#include "net/WireCodec.h"
#include "net/WorldState.h"
#include "render/SimRenderBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fl;

namespace {

struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Feed a ConnectAck (optionally with the granted-authority TLV) into a handler so grantedCaps() is set.
void feedConnectAck(ClientNetEventHandler& h, uint64_t caps) {
    MsgConnectAck ack{};
    ack.grantedRole = static_cast<uint8_t>(PeerRole::Observer);
    ack.peerId = 3;
    std::vector<uint8_t> pkt;
    appendMsg(pkt, ack);
    if (caps != 0) {
        uint8_t payload[sizeof(uint64_t) + sizeof(uint16_t)];
        uint16_t faction = 0xFFFFu;
        std::memcpy(payload, &caps, sizeof(caps));
        std::memcpy(payload + sizeof(caps), &faction, sizeof(faction));
        appendExtRaw(pkt, static_cast<uint16_t>(ExtTag::ConnectAckAuthority), payload, sizeof(payload));
    }
    h.onReceive(0u, pkt.data(), pkt.size());
}

// Feed a one-chunk GM feed with the given records.
void feedGm(ClientNetEventHandler& h, uint64_t tick, const std::vector<GmEntityRecord>& recs) {
    MsgGmWorldStateHeader hdr{};
    hdr.tick = tick;
    hdr.count = static_cast<uint16_t>(recs.size());
    std::vector<uint8_t> pkt;
    appendMsg(pkt, hdr);
    for (const auto& r : recs)
        appendMsg(pkt, r);
    h.onReceive(0u, pkt.data(), pkt.size());
}

GmEntityRecord gmRec(uint32_t idx, uint16_t gen, float x, float z, uint16_t formationId, uint8_t flags) {
    GmEntityRecord r{};
    r.entityIdx = idx;
    r.gen = gen;
    r.pos[0] = x;
    r.pos[2] = z;
    r.formationId = formationId;
    r.flags = flags;
    r.hpPct = 100;
    return r;
}

struct Harness {
    NullLog log;
    SimRenderBridge bridge;
    EntityTypeRegistry registry;
    TrackingNetwork net;
    EnvironmentState env{};
    ClientNetEventHandler handler{bridge, registry, log, net, env};
    NullGui gui;
    std::vector<std::string> sentCommands;

    GmMapOverlay makeOverlay() {
        GmMapOverlay::Deps deps;
        deps.net = &handler;
        deps.registry = &registry;
        deps.gui = &gui;
        deps.serverCommand = [this](std::string_view c) { sentCommands.emplace_back(c); };
        return GmMapOverlay{std::move(deps)};
    }
};

} // namespace

TEST_CASE("GmMapOverlay: click selects the nearest entity on the map (#861)", "[gm_map_overlay]") {
    Harness h;
    feedConnectAck(h.handler, kGameMasterCaps);
    // One entity at the world origin -> screen centre of the map rect.
    feedGm(h.handler, 100, {gmRec(7, 1, 0.f, 0.f, /*formation=*/0, /*flags=*/0)});

    GmMapOverlay ov = h.makeOverlay();
    ov.setOpen(true);

    MockInput input;
    MockWindow window;
    // Click at the map rect centre (map spans kMapX0..kMapX1 / kMapY0..kMapY1 of a 1000x1000 window).
    window.logW = 1000;
    window.logH = 1000;
    const float cx = (0.02f + 0.74f) * 0.5f;
    const float cy = (0.06f + 0.96f) * 0.5f;
    input.mouseX = static_cast<int>(cx * 1000.f);
    input.mouseY = static_cast<int>(cy * 1000.f);
    input.mouseJustPressed.insert(MouseButton::Left);

    ov.update(input, window);
    CHECK(ov.hasSelection());
    CHECK(ov.selectedIdx() == 7u);
}

TEST_CASE("GmMapOverlay: order button on a flighted entity sends a flight order (#861)", "[gm_map_overlay]") {
    Harness h;
    feedConnectAck(h.handler, kGameMasterCaps);
    feedGm(h.handler, 100, {gmRec(4, 1, 0.f, 0.f, /*formation=*/9, /*flags=*/0)}); // AI in flight 9

    GmMapOverlay ov = h.makeOverlay();
    ov.setOpen(true);

    // Select the entity via a centre click.
    MockInput input;
    MockWindow window;
    window.logW = 1000;
    window.logH = 1000;
    input.mouseX = static_cast<int>((0.02f + 0.74f) * 0.5f * 1000.f);
    input.mouseY = static_cast<int>((0.06f + 0.96f) * 0.5f * 1000.f);
    input.mouseJustPressed.insert(MouseButton::Left);
    ov.update(input, window);
    REQUIRE(ov.hasSelection());

    // Now script the "Engage bandits" button click and update again (no mouse press this frame).
    h.gui.buttonClicks["Engage bandits"] = true;
    MockInput input2;
    ov.update(input2, window);

    bool sawOrder = false;
    for (const auto& c : h.sentCommands)
        if (c == "flight order 9 engage_bandits")
            sawOrder = true;
    CHECK(sawOrder);
}

TEST_CASE("GmMapOverlay: View button emits a view request and a spectate command (#861)", "[gm_map_overlay]") {
    Harness h;
    feedConnectAck(h.handler, kGameMasterCaps); // peerId 3
    feedGm(h.handler, 100, {gmRec(5, 2, 0.f, 0.f, 0, 0)});

    GmMapOverlay ov = h.makeOverlay();
    ov.setOpen(true);

    MockInput input;
    MockWindow window;
    window.logW = 1000;
    window.logH = 1000;
    input.mouseX = static_cast<int>((0.02f + 0.74f) * 0.5f * 1000.f);
    input.mouseY = static_cast<int>((0.06f + 0.96f) * 0.5f * 1000.f);
    input.mouseJustPressed.insert(MouseButton::Left);
    ov.update(input, window);
    REQUIRE(ov.hasSelection());

    h.gui.buttonClicks["View from here"] = true;
    MockInput input2;
    ov.update(input2, window);

    // The view request is handed back and the map closed.
    auto req = ov.takeViewRequest();
    REQUIRE(req.has_value());
    CHECK(req->idx == 5u);
    CHECK(req->gen == 2u);
    CHECK_FALSE(ov.isOpen());
    // A spectate command re-centres server interest on the target.
    bool sawSpectate = false;
    for (const auto& c : h.sentCommands)
        if (c == "spectate 3 5")
            sawSpectate = true;
    CHECK(sawSpectate);
}

TEST_CASE("GmMapOverlay: without GmMap authority no order buttons are offered (#861)", "[gm_map_overlay]") {
    Harness h;
    feedConnectAck(h.handler, 0); // no caps
    feedGm(h.handler, 100, {gmRec(4, 1, 0.f, 0.f, /*formation=*/9, 0)});

    GmMapOverlay ov = h.makeOverlay();
    ov.setOpen(true);

    MockInput input;
    MockWindow window;
    window.logW = 1000;
    window.logH = 1000;
    input.mouseX = static_cast<int>((0.02f + 0.74f) * 0.5f * 1000.f);
    input.mouseY = static_cast<int>((0.06f + 0.96f) * 0.5f * 1000.f);
    input.mouseJustPressed.insert(MouseButton::Left);
    ov.update(input, window);

    // Even if the client were to force the order button, an unauthorized panel never renders it.
    h.gui.buttonClicks["Engage bandits"] = true;
    MockInput input2;
    ov.update(input2, window);
    for (const auto& c : h.sentCommands)
        CHECK(c.rfind("flight order", 0) != 0u); // no flight order was sent
    // The "no authority" note is shown instead.
    bool sawNote = false;
    for (const auto& l : h.gui.labels)
        if (l.find("no game-master authority") != std::string::npos)
            sawNote = true;
    CHECK(sawNote);
}
