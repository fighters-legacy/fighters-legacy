// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the widest untrusted-input surface on the game client — ClientNetEventHandler::
// onReceive. A server (or a man-in-the-middle) can send any of the 10 server->client message types:
// Hello, ConnectAck, the quantized WorldSnapshot bitstream + trailing TLV block, WeatherState,
// ServerNotice, AdminResponse, the multi-packet AdminResponseChunk reassembly stream, Motd,
// ConnectRefusal, and PeerDelay. The handler is stood up over the test mocks (no SDL/ENet/Vulkan)
// with a real ServerNotice + GameConsole wired so the notice/console print paths execute; each fuzz
// frame is delivered as one packet. Invariant: no OOB read / no UB under ASan+UBSan.

#include <cstddef>
#include <cstdint>

#include "ClientNetEventHandler.h"
#include "ServerNotice.h"

#include "IClock.h"
#include "ILogger.h"
#include "RenderTypes.h"
#include "console/CommandRegistry.h"
#include "console/GameConsole.h"
#include "entity/EntityTypeRegistry.h"
#include "render/SimRenderBridge.h"

#include "FuzzFrames.h"
#include "mock_network.h"

namespace {

struct SilentLogger : fl::ILogger {
    void log(fl::LogLevel, const char*, int, const char*) override {}
    void setMinLevel(fl::LogLevel) override {}
    void flush() override {}
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fl::SimRenderBridge bridge;
    fl::EntityTypeRegistry registry;
    SilentLogger logger;
    fl::TrackingNetwork net;
    fl::EnvironmentState env{};
    fl::ServerNotice notice;
    fl::CommandRegistry cmdReg;
    fl::GameConsole console(logger, cmdReg);

    fl::ManualClock clock; // fixed: crashes reproduce from a single input file
    fl::ClientNetEventHandler handler(bridge, registry, logger, net, env);
    handler.notice = &notice;   // exercise the banner path
    handler.console = &console; // exercise the console print path (Motd / AdminResponse / notices)
    handler.setClock(clock);

    // Each frame is one server->client packet, delivered in order so the AdminResponseChunk
    // reassembly and the delta-after-full snapshot cache reach their multi-packet states.
    fl::forEachFuzzFrame(data, size, [&](const uint8_t* frame, size_t len) { handler.onReceive(0u, frame, len); });
    return 0;
}
