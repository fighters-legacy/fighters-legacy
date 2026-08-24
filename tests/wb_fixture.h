// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The WorldBroadcaster fixtures a suite needs when it is NOT a broadcaster suite (#1275).
//
// world_broadcaster_test_util.h carries the snapshot decoders, the congestion helpers and the
// interest-shed scenario, and pulls AtcService, ContentBootstrap and JobSystem in with them. Five
// suites wanted only the front half -- a debug EntityDef and the connect handshake -- and their
// link sets cannot take that include, so each re-rolled its own copy. Every copy then froze a
// different vintage of a handshake that has changed shape three times (#853 introduced the
// request, #1049 added the refusal, #1069 dropped the preamble), which is exactly the drift the
// util header was written to prevent -- just one level down.
//
// So the front half lives here, over WorldBroadcaster/GameProtocol/EntityDef/WeaponDef and the
// INTERFACE-only mocks, and world_broadcaster_test_util.h includes it. If you are adding a helper
// that needs a snapshot decoder or a JobSystem, it belongs in that header, not this one.

#include "entity/EntityDef.h"
#include "entity/IEntityController.h"
#include "net/GameProtocol.h"
#include "net/WorldBroadcaster.h"
#include "weapon/WeaponDef.h"

#include "mock_log.h"
#include "mock_network.h"

#include <cstdio>
#include <cstring>
#include <string>

// Test-only, and every consumer already opened with it.
using namespace fl;

// Records broadcasts/sends/disconnects + resolves configurable peer addresses (see mock_network.h).
using MockNetwork = TrackingNetwork;

inline fl::EntityDef makeDebugDef(const char* id = "builtin:debug-entity") {
    fl::EntityDef def;
    def.id = id;
    def.name = "Debug";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;
    return def;
}

// Drive the #853 connect handshake for a PILOT peer: onConnect (which now only sends MsgHello) followed
// by the client's MsgConnectRequest, which is what admits the peer (spawn + MsgConnectAck + MOTD +
// flight check-in). This reproduces the exact net.sends sequence the pre-#853 auto-spawning onConnect
// produced, so it is a drop-in replacement everywhere a test just needs a spawned pilot. If onConnect
// REJECTED the peer (ban / allowlist / rate-limit / per-IP cap / lockout), the only send is a
// MsgConnectRefusal and no request is injected — matching a rejected connection's old behavior exactly.
inline void connectPilotPeer(fl::WorldBroadcaster& b, MockNetwork& net, uint32_t peerId, const char* entityType = "") {
    const std::size_t before = net.sends.size();
    b.onConnect(peerId);
    const bool rejected = net.sends.size() > before && !net.sends.back().empty() &&
                          net.sends.back()[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal);
    if (rejected)
        return;
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    std::snprintf(req.requestedEntityType, sizeof(req.requestedEntityType), "%s", entityType);
    b.onReceive(peerId, &req, sizeof(req));
}

// The same handshake for a suite whose network double is not a TrackingNetwork and therefore has no
// `sends` to inspect (test_admin_console's MockNetworkWb overrides getPeerAddress on NullNetwork).
// Without the send log there is no refusal to detect, so the request is always injected — which is
// what those suites' own copies did. Prefer the overload above wherever the double allows it: a
// refused peer that still gets a request is a silently different scenario.
inline void connectPilotPeer(fl::WorldBroadcaster& b, uint32_t peerId, const char* entityType = "") {
    b.onConnect(peerId);
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    std::snprintf(req.requestedEntityType, sizeof(req.requestedEntityType), "%s", entityType);
    b.onReceive(peerId, &req, sizeof(req));
}

// Drive the #853 handshake for an OBSERVER peer (#857): admitted with a role but NO entity/controller.
// It still receives snapshots (interest centered on its camera eye, #858), so this is the fixture for
// observer interest tests. Returns without injecting a request if onConnect rejected the peer.
inline void connectObserverPeer(fl::WorldBroadcaster& b, MockNetwork& net, uint32_t peerId) {
    const std::size_t before = net.sends.size();
    b.onConnect(peerId);
    const bool rejected = net.sends.size() > before && !net.sends.back().empty() &&
                          net.sends.back()[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal);
    if (rejected)
        return;
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    b.onReceive(peerId, &req, sizeof(req));
}

// The observer handshake without a send log to inspect — see the pilot overload above.
inline void connectObserverPeer(fl::WorldBroadcaster& b, uint32_t peerId) {
    b.onConnect(peerId);
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    b.onReceive(peerId, &req, sizeof(req));
}

inline fl::EntityDef makeCrewBomberDef() {
    fl::EntityDef d;
    d.id = "test:crewbomber";
    d.name = "CrewBomber";
    d.category = fl::ObjectCategory::AirVehicle;
    d.maxHp = 300.f;
    fl::Hardpoint hp0;
    hp0.slot = 0;
    hp0.allowed = {"test:rkt"};
    hp0.defaultWeapon = "test:rkt";
    fl::Hardpoint hp1;
    hp1.slot = 1;
    hp1.allowed = {"test:rkt"};
    hp1.defaultWeapon = "test:rkt";
    d.hardpoints = {hp0, hp1};
    fl::TurretDef t;
    t.id = "tail";
    t.stations = {1};
    d.turrets = {t};
    fl::SeatDef pilot;
    pilot.role = "pilot";
    pilot.capabilities = fl::withCapability(fl::withCapability(fl::CrewCapabilityMask{0}, fl::CrewCapability::Fly),
                                            fl::CrewCapability::Fire);
    pilot.stations = {0};
    fl::SeatDef gunner;
    gunner.role = "tail-gunner";
    gunner.capabilities = fl::withCapability(fl::CrewCapabilityMask{0}, fl::CrewCapability::Fire);
    gunner.turret = "tail";
    d.crew = {pilot, gunner};
    return d;
}

inline fl::WeaponDef makeRktWeapon() {
    fl::WeaponDef w;
    w.id = "test:rkt";
    w.name = "Rkt";
    w.type = fl::WeaponType::Rocket;
    w.category = fl::WeaponCategory::AirToGround;
    w.performance.maxRangeM = 4000.f;
    w.performance.maxSpeedMps = 500.f;
    w.load.massKg = 20.f;
    w.load.rounds = 40;
    return w;
}

struct StillCtl : fl::IEntityController {
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext&) override {
        return fl::ControlInput{};
    }
};

// Stub controller: drives the entity at a fixed throttle with no peer connection. Records how many
// times it is sampled so the test can confirm onTick steps non-peer entities.
struct ConstantController : fl::IEntityController {
    float throttle{1.0f};
    int sampleCount{0};
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext&) override {
        ++sampleCount;
        fl::ControlInput ctrl{};
        ctrl.throttle = throttle;
        return ctrl;
    }
};
