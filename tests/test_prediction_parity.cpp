// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE LOAD-BEARING TEST for #811: the client and the server integrate the SAME aircraft.
//
// What it stops coming back. The client's EntityDef arrives over the wire, and MsgEntityTypeDef used
// to carry no flight model. So the client re-loaded the entity def from disk by its ID --
// "entities/fl-base:f15c.toml", a filename with a colon in it -- missed, swallowed the miss in a
// bare catch (...), and silently predicted every pack aircraft with the builtin UFO model while the
// server integrated the real one. Permanent divergence, nothing logged.
//
// The test drives the REAL server path (WorldBroadcaster::onConnect emits the actual bytes) and the
// REAL client path (makeFlightModelResolver over a registry populated from those bytes), then steps
// both integrators from identical state with identical inputs and asserts they stay together.

#include "ClientFlightModelResolver.h"

#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/IContentPack.h>
#include <entity/EntityDefParser.h>
#include <entity/EntityManager.h>
#include <entity/EntityTypeRegistry.h>
#include <flight/BuiltinFlightModel.h>
#include <flight/FlightIntegrator.h>
#include <flight/FlightModelParser.h>
#include <mock_content.h>
#include <mock_network.h>
#include <net/GameProtocol.h>
#include <net/WireCodec.h>
#include <net/WorldBroadcaster.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace fl;

namespace {

struct QuietLog : public ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// A real, distinctly non-builtin flight model: heavier, draggier and far less powerful than the
// BuiltinFlightModel UFO, so predicting with the wrong one diverges within a second of flight.
const char* kJetFlightModel = R"(
[aircraft]
name         = "Test Jet"
type         = "fighter"
engine_type  = "turbojet"
has_fbw      = false
cruise_alt_m = 11000.0

[flight_model]
mass_kg      = 4349.0
wing_area_m2 = 17.28
wingspan_m   = 8.13
mac_m        = 2.4
fuel_kg      = 2000.0
ixx_kg_m2    = 3800.0
iyy_kg_m2    = 25000.0
izz_kg_m2    = 27000.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0]
mach   = [0.3, 0.9]
values = [
    -0.2, -0.2,
     0.05, 0.05,
     0.4,  0.4,
     0.75, 0.75,
     1.05, 1.05,
]

[aero.drag_polar]
cd0           = 0.020
k             = 0.083
speedbrake_cd = 0.07
gear_cd       = 0.03

[aero.moments]
cm_alpha = -0.7
cm_q     = -10.0
cm_de    = -1.0
cl_beta  = -0.08
cl_p     = -0.40
cl_da    =  0.07
cn_beta  =  0.10
cn_r     = -0.12
cn_dr    = -0.05

[aero.limits]
alpha_stall_deg  = 18.0
max_g_structural = 7.33
min_g_structural = -3.0
max_mach         = 1.63

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.08
fuel_flow_mil_kg_s  = 0.62
fuel_flow_ab_kg_s   = 1.75
spool_time_s        = 4.0

[engine.mil_thrust]
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [15.6, 7.0,
          14.0, 8.0]
)";

std::string entityToml() {
    return "[entity]\n"
           "id = \"fl-base:testjet\"\n"
           "name = \"Test Jet\"\n"
           "category = \"air_vehicle\"\n"
           "max_hp = 100.0\n"
           "mesh = \"testjet\"\n"
           "flight_model = \"testjet\"\n"; // ASSET NAME (a file), not a def id
}

// Serves the flight model as a real content-pack asset.
struct JetPack : public NullContentPack {
    std::map<std::string, std::string> flightModels;

    bool hasAsset(const char* n, AssetType t) const override {
        return t == AssetType::FlightModel && flightModels.count(n) != 0;
    }
    std::optional<FlightModel> loadFlightModel(const char* n) override {
        auto it = flightModels.find(n);
        if (it == flightModels.end())
            return std::nullopt;
        FlightModel d;
        d.name = n;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
    std::vector<std::string> listAssets(AssetType t) const override {
        std::vector<std::string> out;
        if (t == AssetType::FlightModel)
            for (const auto& [k, v] : flightModels)
                out.push_back(k);
        return out;
    }
};

std::unique_ptr<AssetManager> makeAssets(ILogger& log) {
    JetPack pack;
    pack.flightModels["testjet"] = kJetFlightModel;
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<JetPack>(std::move(pack)));
    auto am = std::make_unique<AssetManager>(std::move(packs), log);
    am->initialize(nullptr);
    return am;
}

// Populates a client-side EntityTypeRegistry from the MsgEntityTypeDef records the server ACTUALLY
// sent, mirroring ClientNetEventHandler's ConnectAck arm.
void registerFromWire(const std::vector<uint8_t>& pkt, EntityTypeRegistry& registry) {
    MsgConnectAck ack{};
    REQUIRE(readMsg(pkt.data(), pkt.size(), ack));

    std::size_t off = sizeof(MsgConnectAck);
    for (uint16_t i = 0; i < ack.typeCount; ++i) {
        MsgEntityTypeDef td;
        REQUIRE(readRecordAt(pkt.data(), pkt.size(), off, td));
        off += sizeof(td);
        td.id[sizeof(td.id) - 1] = '\0';
        td.mesh[sizeof(td.mesh) - 1] = '\0';
        td.dmgMesh[sizeof(td.dmgMesh) - 1] = '\0';
        td.flightModel[sizeof(td.flightModel) - 1] = '\0';

        EntityDef def;
        def.id = td.id;
        def.mesh = td.mesh;
        def.classicDamageMesh = td.dmgMesh;
        def.flightModelAsset = td.flightModel;
        def.payloadMassKg = td.payloadMassKg;
        def.payloadCd0 = td.payloadCd0;
        def.maxHp = 100.0f;
        registry.registerType(std::move(def));
    }
}

// The ConnectAck is the first reliable packet whose msgId says so.
const std::vector<uint8_t>* findConnectAck(const TrackingNetwork& net) {
    for (const auto& pkt : net.sends)
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(MsgId::ConnectAck))
            return &pkt;
    return nullptr;
}

} // namespace

TEST_CASE("MsgEntityTypeDef carries the flight model asset name to the client", "[prediction_parity]") {
    QuietLog log;
    TrackingNetwork net;
    EntityTypeRegistry serverRegistry;
    EntityManager em(log, serverRegistry);

    EntityDef def = parseEntityDef(entityToml());
    serverRegistry.registerType(def);

    WorldBroadcaster broadcaster(em, serverRegistry, net, log);
    broadcaster.onConnect(0u);

    const std::vector<uint8_t>* ack = findConnectAck(net);
    REQUIRE(ack != nullptr);

    EntityTypeRegistry clientRegistry;
    registerFromWire(*ack, clientRegistry);

    const EntityDef* clientDef = clientRegistry.findById("fl-base:testjet");
    REQUIRE(clientDef != nullptr);
    CHECK(clientDef->flightModelAsset == "testjet"); // the field that did not exist before #811
}

TEST_CASE("client and server resolve the same flight model for the same entity type", "[prediction_parity]") {
    QuietLog log;
    TrackingNetwork net;
    EntityTypeRegistry serverRegistry;
    EntityManager em(log, serverRegistry);
    serverRegistry.registerType(parseEntityDef(entityToml()));

    auto assets = makeAssets(log);

    WorldBroadcaster broadcaster(em, serverRegistry, net, log);
    broadcaster.onConnect(0u);
    const std::vector<uint8_t>* ack = findConnectAck(net);
    REQUIRE(ack != nullptr);

    EntityTypeRegistry clientRegistry;
    registerFromWire(*ack, clientRegistry);

    // Server: resolves EntityDef::flightModelAsset through the AssetManager (fl-server's resolver).
    const EntityDef* serverDef = serverRegistry.byIndex(0);
    REQUIRE(serverDef != nullptr);
    auto rawFm = assets->loadFlightModel(serverDef->flightModelAsset.c_str());
    REQUIRE(rawFm != nullptr);
    auto serverModel = std::make_shared<const FlightModelData>(
        parseFlightModel(std::string_view(reinterpret_cast<const char*>(rawFm->bytes.data()), rawFm->bytes.size())));

    // Client: resolves it from the wire-built registry.
    auto resolver = makeFlightModelResolver(clientRegistry, *assets, log);
    std::shared_ptr<const FlightModelData> clientModel = resolver(0);
    REQUIRE(clientModel != nullptr);

    // It is the PACK's model, not the builtin UFO. This is the assertion that would have failed
    // before #811 -- and failed silently, in the game, forever.
    CHECK(clientModel.get() != BuiltinFlightModel::get().get());
    CHECK(clientModel->meta.name == "Test Jet");

    CHECK_THAT(clientModel->geometry.mass_kg, WithinAbs(serverModel->geometry.mass_kg, 1e-4f));
    CHECK_THAT(clientModel->geometry.wing_area_m2, WithinAbs(serverModel->geometry.wing_area_m2, 1e-4f));
    CHECK_THAT(clientModel->drag_polar.cd0, WithinAbs(serverModel->drag_polar.cd0, 1e-6f));
    REQUIRE(clientModel->engine.mil_thrust.values.size() == serverModel->engine.mil_thrust.values.size());
    for (size_t i = 0; i < clientModel->engine.mil_thrust.values.size(); ++i)
        CHECK_THAT(clientModel->engine.mil_thrust.values[i],
                   WithinAbs(serverModel->engine.mil_thrust.values[i], 1e-4f));
}

TEST_CASE("client and server integrators do not diverge over 600 ticks", "[prediction_parity]") {
    QuietLog log;
    TrackingNetwork net;
    EntityTypeRegistry serverRegistry;
    EntityManager em(log, serverRegistry);
    serverRegistry.registerType(parseEntityDef(entityToml()));

    auto assets = makeAssets(log);

    WorldBroadcaster broadcaster(em, serverRegistry, net, log);
    broadcaster.onConnect(0u);
    const std::vector<uint8_t>* ack = findConnectAck(net);
    REQUIRE(ack != nullptr);

    EntityTypeRegistry clientRegistry;
    registerFromWire(*ack, clientRegistry);

    const EntityDef* serverDef = serverRegistry.byIndex(0);
    auto rawFm = assets->loadFlightModel(serverDef->flightModelAsset.c_str());
    REQUIRE(rawFm != nullptr);
    auto serverModel = std::make_shared<const FlightModelData>(
        parseFlightModel(std::string_view(reinterpret_cast<const char*>(rawFm->bytes.data()), rawFm->bytes.size())));

    auto clientModel = makeFlightModelResolver(clientRegistry, *assets, log)(0);
    REQUIRE(clientModel != nullptr);

    // Identical initial state, identical inputs, ten seconds of flight.
    auto seed = [](FlightIntegrator& fi, const FlightModelData& m) {
        FlightState s{};
        s.pos_world[0] = 0.0;
        s.pos_world[1] = 5000.0;
        s.pos_world[2] = 0.0;
        s.quat[3] = 1.f; // identity
        s.vel_body[0] = 250.0;
        s.mass_kg = m.geometry.mass_kg + m.geometry.fuel_kg;
        s.fuel_kg = m.geometry.fuel_kg;
        fi.reset(s);
    };

    FlightIntegrator server(serverModel);
    FlightIntegrator client(clientModel);
    seed(server, *serverModel);
    seed(client, *clientModel);

    ControlInput ctrl{};
    ctrl.throttle = 0.85f;
    ctrl.elevator = 0.10f;
    ctrl.aileron = 0.05f;

    constexpr float kDt = 1.f / 60.f;
    for (int tick = 0; tick < 600; ++tick) {
        server.step(kDt, ctrl, {}, {}, 0.f);
        client.step(kDt, ctrl, {}, {}, 0.f);
    }

    const FlightState& a = server.state();
    const FlightState& b = client.state();
    const double dx = a.pos_world[0] - b.pos_world[0];
    const double dy = a.pos_world[1] - b.pos_world[1];
    const double dz = a.pos_world[2] - b.pos_world[2];
    const double divergence = std::sqrt(dx * dx + dy * dy + dz * dz);

    INFO("positional divergence after 600 ticks: " << divergence << " m");
    CHECK(divergence < 1e-3); // same model, same inputs => bit-for-bit the same aeroplane

    // Sanity: the aircraft actually went somewhere, so the assertion above is not vacuous.
    CHECK(std::abs(a.pos_world[0]) > 100.0);
}

TEST_CASE("a missing flight model logs at Error and names the id", "[prediction_parity]") {
    struct CountingLog : public ILogger {
        std::vector<std::pair<LogLevel, std::string>> entries;
        void log(LogLevel lvl, const char*, int, const char* msg) override {
            entries.push_back({lvl, msg ? msg : ""});
        }
        void setMinLevel(LogLevel) override {}
        void flush() override {}
    } log;

    // A pack with NO flight models at all: the entity names one that nobody provides.
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<JetPack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    EntityTypeRegistry registry;
    registry.registerType(parseEntityDef(entityToml()));

    auto model = makeFlightModelResolver(registry, assets, log)(0);

    CHECK(model.get() == BuiltinFlightModel::get().get()); // still flies -- but LOUDLY
    bool named = false;
    for (const auto& [lvl, msg] : log.entries)
        if (lvl == LogLevel::Error && msg.find("fl-base:testjet") != std::string::npos &&
            msg.find("testjet") != std::string::npos)
            named = true;
    CHECK(named);
}
