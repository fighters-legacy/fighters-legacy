// SPDX-License-Identifier: GPL-3.0-or-later
//
// Base operations (#55): the "base refuel|rearm|repair" radio verbs — server-authoritative
// ground-crew services. WB-level, like the surface-threat tests: the point is command -> validate
// (shut down at a base) -> apply -> crew-chief radio reply.
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "content/ContentBootstrap.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "net/WorldBroadcaster.h"
#include "weapon/WeaponRegistry.h"

#include "mock_network.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace fl;

namespace {

struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

struct BaseOpsFixture {
    NullLog logger;
    TrackingNetwork net;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    std::unique_ptr<EntityManager> em;
    // Set before the fixture body builds the broadcaster: queries are frozen at construction (#1082).
    fl::WorldQueries queries;
    std::unique_ptr<WorldBroadcaster> wb;
    uint64_t t{0};

    // The base-proximity query is frozen at construction (#1082), so a test that wants one passes it
    // in rather than setting it afterwards.
    explicit BaseOpsFixture(bool spawnOnGround, std::function<bool(glm::dvec3)> baseProximity = {}) {
        queries.baseProximity = std::move(baseProximity);
        registry.registerType(builtinDebugEntityDef());
        registerBuiltinWeapons(weapons);

        em = std::make_unique<EntityManager>(logger, registry);
        wb = std::make_unique<WorldBroadcaster>(*em, registry, net, logger, nullptr, std::move(queries));
        wb->setWeaponRegistry(&weapons);
        wb->setGroundElevation(0.f);
        if (spawnOnGround)
            wb->setSpawnPoints({{0.0, 0.4, 0.0}}); // on the ramp; default is 500 m AGL (airborne)

        // #853 handshake: connect, then the pilot's ConnectRequest spawns the aircraft.
        wb->onConnect(7u);
        MsgConnectRequest req{};
        req.requestedRole = static_cast<uint8_t>(PeerRole::Pilot);
        wb->onReceive(7u, &req, sizeof(req));
        tick(30); // settle (parking hold latches a grounded spawn fully static)
    }

    void tick(int n) {
        for (int i = 0; i < n; ++i)
            wb->onTick(1.0 / 60.0, ++t);
    }

    EntityId pilotEntity() {
        EntityId id{};
        wb->forEachPeer([&](const PeerInfo& info) {
            if (info.peerId == 7u)
                id = info.eid;
        });
        return id;
    }

    void sendBaseOp(const char* op) {
        MsgRadioCommand msg{};
        std::snprintf(msg.command, sizeof(msg.command), "base %s", op);
        wb->onReceive(7u, &msg, sizeof(msg));
    }

    // The last radio line unicast to peer 7, or empty.
    std::string lastRadioText() const {
        for (auto it = net.perPeerSends.rbegin(); it != net.perPeerSends.rend(); ++it) {
            if (it->first != 7u || it->second.empty())
                continue;
            if (it->second[0] != static_cast<uint8_t>(MsgId::RadioTransmission))
                continue;
            MsgRadioTransmission w{};
            if (it->second.size() < sizeof(w))
                continue;
            std::memcpy(&w, it->second.data(), sizeof(w));
            w.text[sizeof(w.text) - 1] = '\0';
            return w.text;
        }
        return {};
    }
};

} // namespace

TEST_CASE("Base ops: repair restores a damaged aircraft shut down on the ramp (#55)", "[base_ops]") {
    BaseOpsFixture f(/*spawnOnGround=*/true);
    const EntityId id = f.pilotEntity();
    REQUIRE(id.valid());

    f.em->applyDamage(id, 60.f, EntityId::null());
    f.tick(1);
    REQUIRE(f.em->get(id)->hp < f.em->get(id)->maxHp);

    f.sendBaseOp("repair");
    CHECK(f.em->get(id)->hp == f.em->get(id)->maxHp);
    CHECK(f.em->get(id)->damageLevel == DamageLevel::Intact);
    CHECK(f.lastRadioText().find("patched up") != std::string::npos);
}

TEST_CASE("Base ops: refuel and rearm answer on the crew-chief radio (#55)", "[base_ops]") {
    BaseOpsFixture f(/*spawnOnGround=*/true);
    REQUIRE(f.pilotEntity().valid());

    f.sendBaseOp("refuel");
    CHECK(f.lastRadioText().find("topped off") != std::string::npos);
    f.sendBaseOp("rearm");
    CHECK(f.lastRadioText().find("rearmed") != std::string::npos);
}

TEST_CASE("Base ops: refused while airborne (#55)", "[base_ops]") {
    BaseOpsFixture f(/*spawnOnGround=*/false); // default spawn: 500 m AGL
    REQUIRE(f.pilotEntity().valid());

    f.sendBaseOp("repair");
    CHECK(f.lastRadioText().find("shut down") != std::string::npos);
    // ...and nothing was repaired-by-accident (nothing to check hp-wise; the refusal is the contract).
}

TEST_CASE("Base ops: refused away from any base when a proximity query is set (#55)", "[base_ops]") {
    BaseOpsFixture f(/*spawnOnGround=*/true, [](glm::dvec3) { return false; }); // nowhere is a base

    f.sendBaseOp("refuel");
    CHECK(f.lastRadioText().find("Get to a base") != std::string::npos);
}

TEST_CASE("Base ops: an unknown op gets a say-again (#55)", "[base_ops]") {
    BaseOpsFixture f(/*spawnOnGround=*/true);
    f.sendBaseOp("wash");
    CHECK(f.lastRadioText().find("say again") != std::string::npos);
}
