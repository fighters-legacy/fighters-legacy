// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the server's CONNECT path and everything gated behind a completed handshake.
//
// fuzz_server_msg calls onConnect(0) and stops there, so two large areas of untrusted-input surface
// were unreachable from it (#1073):
//
//   1. handleConnectRequest, the LARGEST untrusted parser in the server. It walks a
//      packCount x PackManifestEntry record loop and then three TLV parses at an ATTACKER-CONTROLLED
//      offset — extOff = sizeof(MsgConnectRequest) + packCount * 128, computed in two places. Nothing
//      reached it, because fuzz_server_msg's seeds never sent a MsgConnectRequest.
//   2. Every handler behind `handshakeComplete` — chat, voice, seat, team, and since #1069 the whole
//      dispatch chain. A harness that never completes the handshake cannot enter any of them.
//
// So this harness does the opposite of fuzz_server_msg's setup: it feeds the FIRST fuzz frame to
// handleConnectRequest as a MsgConnectRequest (so the parser sees attacker-shaped bytes in the one
// message that is legal pre-handshake), then admits a SECOND peer through a known-good handshake and
// feeds it every remaining frame — so the post-handshake handlers are exercised with a peer that is
// genuinely admitted rather than one that got in because nothing checked.
//
// The radio-net table is populated so selectVoiceRecipients and the relay fan-out actually execute;
// with an empty table handleVoiceFrame returns at recipient selection and the fan-out is never fuzzed.
//
// Invariant: no OOB read / no UB under ASan+UBSan for any attacker-controlled buffer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "IClock.h"
#include "ILogger.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "net/WorldBroadcaster.h"
#include "voice/RadioNet.h"

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
    def.id = "builtin:debug-entity";
    def.name = "Debug";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;
    return def;
}

// The peer whose handshake is driven by fuzz bytes: handleConnectRequest sees whatever the first
// frame contains, so packCount, the callsign and the TLV block are all attacker-shaped.
constexpr uint32_t kFuzzedPeer = 0;
// The peer admitted through a known-good handshake, used for every post-handshake handler. Kept
// separate so a malformed connect request cannot accidentally close the door on the frames that are
// meant to reach chat / voice / seat / team.
constexpr uint32_t kAdmittedPeer = 1;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    SilentLogger logger;
    fl::TrackingNetwork net;
    net.peerAddresses[kFuzzedPeer] = "203.0.113.7";
    net.peerAddresses[kAdmittedPeer] = "203.0.113.8";

    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::ManualClock clock; // fixed: crashes reproduce from a single input file
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    broadcaster.setBuildVersion("0.0.0-fuzz");

    // Voice routing (#532): without a net table handleVoiceFrame stops at recipient selection, so the
    // relay fan-out — the part that walks every peer — would never be entered. builtinRadioNets() is
    // the same stack a server with no [[voice.nets]] configuration runs.
    broadcaster.setVoiceEnabled(true);
    {
        fl::RadioNetTable nets;
        for (fl::RadioNetDef& def : fl::builtinRadioNets())
            nets.add(std::move(def));
        broadcaster.setRadioNets(std::move(nets));
    }

    // Chat is on by default in fl-server; turn it on explicitly so the handler is reachable here too.
    broadcaster.setChatEnabled(true);

    broadcaster.onConnect(kFuzzedPeer);
    broadcaster.onConnect(kAdmittedPeer);

    // Admit the second peer through a well-formed handshake, so the post-handshake handlers below run
    // against a genuinely admitted peer.
    {
        fl::MsgConnectRequest req{};
        req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
        broadcaster.onReceive(kAdmittedPeer, &req, sizeof(req));
    }

    uint64_t tick = 1;
    bool firstFrame = true;
    fl::forEachFuzzFrame(data, size, [&](const uint8_t* frame, size_t len) {
        if (firstFrame) {
            firstFrame = false;
            // Force the first frame down the connect path regardless of its msgId byte: the point is
            // to reach handleConnectRequest's record loop and TLV parsing with fuzzed bytes, and a
            // mutation that flips byte 0 would otherwise divert the whole input elsewhere.
            std::vector<uint8_t> req(frame, frame + len);
            if (!req.empty())
                req[0] = static_cast<uint8_t>(fl::MsgId::ConnectRequest);
            broadcaster.onReceive(kFuzzedPeer, req.data(), req.size());
        } else {
            broadcaster.onReceive(kAdmittedPeer, frame, len);
        }
        broadcaster.onTick(1.0 / 60.0, tick++);
    });
    // A final tick so anything queued by the last frame is drained + broadcast.
    broadcaster.onTick(1.0 / 60.0, tick);
    return 0;
}
