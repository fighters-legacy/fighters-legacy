// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the authoritative server's untrusted-input path — WorldBroadcaster::onReceive.
// This is what a connected (or spoofing) client can send: MsgClientInput, MsgAdminCommand, and
// MsgHeartbeat, plus any unknown/short byte blob. A real WorldBroadcaster is stood up over the
// test mocks (no SDL/ENet), one peer is connected, and each fuzz frame is delivered as a packet
// followed by a sim tick so the buffered-input / ack / snapshot-build state is actually consumed.
// The invariant: no OOB read / no UB under ASan+UBSan for any attacker-controlled buffer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "IClock.h"
#include "ILogger.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "net/WorldBroadcaster.h"

#include "FuzzFrames.h"
#include "mock_network.h"

namespace {

struct SilentLogger : fl::ILogger {
    void log(fl::LogLevel, const char*, int, const char*) override {}
    void setMinLevel(fl::LogLevel) override {}
    void flush() override {}
};

fl::EntityDef makeDebugDef() {
    fl::EntityDef def;
    def.id = "builtin:debug-entity"; // onConnect spawns this per-peer entity
    def.name = "Debug";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;
    return def;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    SilentLogger logger;
    fl::TrackingNetwork net;
    net.peerAddresses[0] = "203.0.113.7"; // give peer 0 an IP so the admin-auth lockout path is reachable

    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::ManualClock clock; // fixed: crashes reproduce from a single input file
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    broadcaster.setOperatorPassword("fz"); // enable the admin channel (dispatch reachable via a seed's token)
    broadcaster.setAdminDispatch([](std::string_view cmd) { return std::string(cmd); });
    broadcaster.onConnect(0u); // peer required for the per-peer unicast paths

    uint64_t tick = 1;
    fl::forEachFuzzFrame(data, size, [&](const uint8_t* frame, size_t len) {
        broadcaster.onReceive(0u, frame, len);
        broadcaster.onTick(1.0 / 60.0, tick++); // drain jitter buffer, build+broadcast a snapshot
    });
    return 0;
}
