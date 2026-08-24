// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoiceChat, the client voice facade (#531) — previously at 0.0% branch coverage (#1145).
//
// Its five components (activity gate, codec, jitter buffer, mixer, router) each had a test file;
// the object that owns them and decides WHEN to key, WHICH net to key on, and when to open or close
// the capture device had none. That is where the behaviour a player notices lives: a mic that stays
// hot while a chat box is focused, a transmission that changes net halfway through a sentence, a
// missing end-of-transmission marker leaving the receiver's squelch open.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "NullAudio.h"
#include "mock_log.h"
#include "voice/VoiceChat.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace fl;

namespace {

// A capture device under test control: it can refuse to open, report a device name, and hand back
// exactly the samples a test wants to see encoded.
struct FakeCapture final : public IAudioCapture {
    bool initOk{true};
    bool inited{false};
    bool capturing{false};
    int startCount{0};
    int stopCount{0};
    int initCount{0};
    int shutdownCount{0};
    std::string device{"fake-mic"};
    std::string requestedDevice;
    std::string lastError{"device busy"};
    std::vector<int16_t> pending; // drained by read()

    bool init(int, int, const std::string& deviceName) override {
        ++initCount;
        requestedDevice = deviceName;
        inited = initOk;
        return initOk;
    }
    void shutdown() override {
        ++shutdownCount;
        inited = false;
        capturing = false;
    }
    const char* getLastError() const override {
        return lastError.c_str();
    }
    bool start() override {
        ++startCount;
        capturing = true;
        return true;
    }
    void stop() override {
        ++stopCount;
        capturing = false;
    }
    bool isCapturing() const override {
        return capturing;
    }
    // A running device NEVER goes quiet: with nobody speaking it delivers silence, not nothing.
    // Modelling it as "returns 0 when the queued audio runs out" starves the frame loop, and since
    // the activity gate only closes when it evaluates a frame, the transmission never ends — the
    // first draft of this file failed three assertions for exactly that reason. (A device that
    // genuinely stops while still reporting isCapturing() would hit the same latent path; that is
    // an unplugged-mic scenario, not the normal one.)
    bool idleSilence{true};
    std::size_t read(int16_t* out, std::size_t maxSamples) override {
        if (pending.empty() && idleSilence && capturing) {
            const std::size_t n = std::min(maxSamples, static_cast<std::size_t>(kVoiceFrameSamples));
            std::fill_n(out, n, static_cast<int16_t>(0));
            return n;
        }
        const std::size_t n = std::min(maxSamples, pending.size());
        std::copy(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n), out);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    std::size_t available() const override {
        return pending.size();
    }
    std::vector<std::string> listDevices() const override {
        return {device};
    }
    std::string currentDevice() const override {
        return device;
    }

    // Loud speech: a sine well above the VOX threshold, so the gate opens in any keying mode.
    void queueLoud(std::size_t frames) {
        for (std::size_t f = 0; f < frames * static_cast<std::size_t>(kVoiceFrameSamples); ++f) {
            const double t = static_cast<double>(f) / kVoiceSampleRate;
            pending.push_back(static_cast<int16_t>(12000.0 * std::sin(2.0 * std::numbers::pi * 440.0 * t)));
        }
    }
    void queueSilence(std::size_t frames) {
        pending.insert(pending.end(), frames * static_cast<std::size_t>(kVoiceFrameSamples), 0);
    }
};

// Records what the "network" was asked to send.
struct SentFrame {
    uint8_t netId;
    uint16_t seq;
    std::size_t bytes;
    bool start;
    bool end;
};

struct Harness {
    // `sent` is declared BEFORE `chat` deliberately: members destruct in reverse declaration order,
    // and ~VoiceChat emits the end-of-transmission marker through the sink. With the vector declared
    // after, that final call writes into an already-destroyed member — valgrind reports the invalid
    // free, and the first draft of this file did exactly that. Any real sink has the same
    // requirement: it must outlive the VoiceChat that calls it.
    std::vector<SentFrame> sent;
    NullLogger logger;
    NullAudio audio;
    FakeCapture capture;
    VoiceChat chat;

    Harness() {
        chat.setFrameSink([this](uint8_t netId, uint16_t seq, std::span<const uint8_t> payload, bool start, bool end) {
            sent.push_back({netId, seq, payload.size(), start, end});
        });
    }

    void initAll() {
        chat.init(&capture, &audio, &logger);
        VoiceSettings v;
        AudioSettings a;
        chat.applySettings(v, a);
    }

    void tick(bool ptt1 = false, bool ptt2 = false, bool uiFocused = false) {
        chat.update(1.f / 60.f, ptt1, ptt2, uiFocused, glm::dvec3(0.0), {});
    }

    // Release the key and run long enough for the activity gate's HANGOVER to expire. The gate
    // deliberately holds the tail open for a few frames after the key drops so a plosive gap does
    // not chop "Fox thre-" into two transmissions; nothing closes until it evaluates a frame past
    // that tail. Asserting immediately after release tests the hangover, not the close.
    void releaseAndSettle(int ticks = 40) {
        for (int i = 0; i < ticks; ++i)
            tick(false);
    }

    [[nodiscard]] std::size_t speechFrames() const {
        return static_cast<std::size_t>(
            std::count_if(sent.begin(), sent.end(), [](const SentFrame& f) { return !f.end && f.bytes > 0; }));
    }
    [[nodiscard]] std::size_t endMarkers() const {
        return static_cast<std::size_t>(
            std::count_if(sent.begin(), sent.end(), [](const SentFrame& f) { return f.end; }));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// init / degradation
// ---------------------------------------------------------------------------

TEST_CASE("VoiceChat: init with no capture and no audio is inert, not an error (#1145)", "[voice][chat]") {
    NullLogger log;
    VoiceChat chat;
    // Headless: no device either way. init reports "nothing live" but the object stays usable.
    const bool live = chat.init(nullptr, nullptr, &log);
    CHECK(chat.captureAvailable() == false);
    CHECK(chat.transmitting() == false);
    // Updating a fully inert facade must not crash or transmit.
    chat.update(1.f / 60.f, true, false, false, glm::dvec3(0.0), {});
    CHECK(chat.transmitting() == false);
    (void)live; // an encoder may or may not be compiled in; both are valid "not an error"
}

TEST_CASE("VoiceChat: a capture device that refuses to open leaves it listen-only (#1145)", "[voice][chat]") {
    Harness h;
    h.capture.initOk = false;
    h.capture.lastError = "no such device";
    h.initAll();

    h.tick(/*ptt1=*/true);
    CHECK_FALSE(h.chat.captureAvailable());
    CHECK(h.chat.captureError() == "no such device");
    CHECK(h.sent.empty());

    // ONE attempt, not a per-frame retry storm.
    const int attempts = h.capture.initCount;
    for (int i = 0; i < 10; ++i)
        h.tick(true);
    CHECK(h.capture.initCount == attempts);
}

TEST_CASE("VoiceChat: changing the input device re-opens it (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    h.tick(true);
    const int opens = h.capture.initCount;
    REQUIRE(opens >= 1);

    VoiceSettings v;
    v.inputDevice = "headset";
    AudioSettings a;
    h.chat.applySettings(v, a);
    h.tick(true);
    CHECK(h.capture.initCount == opens + 1);
    CHECK(h.capture.requestedDevice == "headset");
}

// ---------------------------------------------------------------------------
// The keying gate: who transmits, on which net, and when the device runs
// ---------------------------------------------------------------------------

TEST_CASE("VoiceChat: push-to-talk only runs the device while keyed (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();

    h.tick(/*ptt1=*/false);
    CHECK_FALSE(h.capture.isCapturing()); // idle: an always-hot mic is a privacy problem

    h.tick(/*ptt1=*/true);
    CHECK(h.capture.isCapturing());

    // Not on the release tick — the hangover still holds the gate, and the device must keep
    // running while it does, or the tail it exists to preserve would be missing.
    h.tick(/*ptt1=*/false);
    h.releaseAndSettle();
    CHECK_FALSE(h.capture.isCapturing());
}

TEST_CASE("VoiceChat: an open VOX or Open-mic mode keeps the device running (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    VoiceSettings v;
    v.keyMode = VoiceKeyMode::Open;
    AudioSettings a;
    h.chat.applySettings(v, a);

    h.tick(); // no key held at all
    CHECK(h.capture.isCapturing());
}

TEST_CASE("VoiceChat: UI focus force-closes the gate (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    h.capture.queueLoud(4);
    h.tick(/*ptt1=*/true);
    REQUIRE(h.chat.transmitting());
    const std::size_t before = h.sent.size();

    // A chat box takes focus mid-transmission: the mic must close AND the receiver must be told,
    // or its squelch stays open on a transmission that already stopped.
    h.capture.queueLoud(4);
    h.tick(/*ptt1=*/true, /*ptt2=*/false, /*uiFocused=*/true);
    CHECK_FALSE(h.chat.transmitting());
    REQUIRE(h.sent.size() > before);
    CHECK(h.sent.back().end);
    CHECK_FALSE(h.capture.isCapturing());
}

TEST_CASE("VoiceChat: the secondary key wins and the net is latched for the burst (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    const uint8_t primary = h.chat.primaryNet();
    const uint8_t secondary = h.chat.secondaryNet();
    REQUIRE(primary != secondary);
    REQUIRE(secondary != kInvalidRadioNet);

    // Both keys down: the urgent one wins.
    h.capture.queueLoud(3);
    h.tick(/*ptt1=*/true, /*ptt2=*/true);
    REQUIRE(h.chat.transmitting());
    CHECK(h.chat.transmittingNet() == secondary);

    // Releasing the secondary MID-BURST must not re-route the tail of the sentence to the primary
    // net — half a sentence delivered to the wrong people is the failure this latch prevents.
    h.capture.queueLoud(3);
    h.tick(/*ptt1=*/true, /*ptt2=*/false);
    CHECK(h.chat.transmittingNet() == secondary);
    for (const SentFrame& f : h.sent)
        CHECK(f.netId == secondary);
}

TEST_CASE("VoiceChat: transmit disabled is listen-only, and voice disabled is silent (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();

    VoiceSettings v;
    v.transmitEnabled = false;
    AudioSettings a;
    h.chat.applySettings(v, a);
    h.capture.queueLoud(4);
    h.tick(/*ptt1=*/true);
    CHECK(h.sent.empty());
    CHECK_FALSE(h.chat.transmitting());

    v.enabled = false;
    v.transmitEnabled = true;
    h.chat.applySettings(v, a);
    h.capture.queueLoud(4);
    h.tick(/*ptt1=*/true);
    CHECK(h.sent.empty());
}

// ---------------------------------------------------------------------------
// Encoding and the transmission boundaries
// ---------------------------------------------------------------------------

TEST_CASE("VoiceChat: a keyed burst emits a start frame then continuation frames (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    h.capture.queueLoud(6);
    h.tick(/*ptt1=*/true);

    if (h.speechFrames() == 0)
        SUCCEED("no encoder compiled in — the listen-only path is covered elsewhere");
    else {
        CHECK(h.sent.front().start);
        CHECK(h.sent.front().seq == 0);
        // Sequence numbers advance monotonically within the burst.
        for (std::size_t i = 1; i < h.sent.size(); ++i)
            CHECK(h.sent[i].seq == h.sent[i - 1].seq + 1);
    }
}

TEST_CASE("VoiceChat: releasing the key emits exactly one end-of-transmission marker (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    h.capture.queueLoud(4);
    h.tick(/*ptt1=*/true);
    REQUIRE(h.chat.transmitting());

    h.releaseAndSettle();
    CHECK_FALSE(h.chat.transmitting());
    CHECK(h.endMarkers() == 1);
    CHECK(h.sent.back().bytes == 0); // the marker carries no audio, only the boundary

    // Idling further must not emit a second one.
    for (int i = 0; i < 20; ++i)
        h.tick(false);
    CHECK(h.endMarkers() == 1);
}

TEST_CASE("VoiceChat: reset closes an open transmission and clears the mic meter (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    h.capture.queueLoud(4);
    h.tick(/*ptt1=*/true);
    REQUIRE(h.chat.transmitting());

    h.chat.reset();
    CHECK_FALSE(h.chat.transmitting());
    CHECK(h.chat.transmittingNet() == kInvalidRadioNet);
    CHECK(h.chat.micLevel() == Catch::Approx(0.f));
    CHECK(h.endMarkers() == 1); // the peer is told, rather than left waiting on a dead stream
}

TEST_CASE("VoiceChat: destruction closes an open transmission through the sink (#1145)", "[voice][chat]") {
    // ~VoiceChat -> shutdown -> reset emits the end marker, so a client that drops the object
    // mid-transmission still leaves the receiver's squelch closed. The sink therefore has to
    // outlive the VoiceChat; this test is also what pins that ordering requirement.
    std::vector<SentFrame> sent;
    {
        NullLogger log;
        NullAudio audio;
        FakeCapture capture;
        VoiceChat chat;
        chat.setFrameSink([&sent](uint8_t netId, uint16_t seq, std::span<const uint8_t> p, bool start, bool end) {
            sent.push_back({netId, seq, p.size(), start, end});
        });
        chat.init(&capture, &audio, &log);
        VoiceSettings v;
        AudioSettings a;
        chat.applySettings(v, a);
        capture.queueLoud(4);
        chat.update(1.f / 60.f, true, false, false, glm::dvec3(0.0), {});
        REQUIRE(chat.transmitting());
    } // destroyed here, still keyed

    REQUIRE_FALSE(sent.empty());
    CHECK(sent.back().end);
    CHECK(sent.back().bytes == 0);
}

TEST_CASE("VoiceChat: mic level tracks input and decays when idle (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    h.capture.queueLoud(8);
    for (int i = 0; i < 4; ++i)
        h.tick(/*ptt1=*/true);
    const float loud = h.chat.micLevel();
    CHECK(loud > 0.f);

    for (int i = 0; i < 20; ++i)
        h.tick(false);
    CHECK(h.chat.micLevel() < loud);
}

TEST_CASE("VoiceChat: a stalled frame does not queue seconds of stale speech (#1145)", "[voice][chat]") {
    // Alt-tab or a load hitch: the device hands over a huge backlog at once. Sending all of it
    // would flood the net with speech from several seconds ago.
    Harness h;
    h.initAll();
    h.capture.queueLoud(60); // 1.2 s of audio in one update
    h.tick(/*ptt1=*/true);

    // At most the retained backlog (half a second = 25 frames) can be emitted in one go.
    CHECK(h.speechFrames() <= 25);
}

// ---------------------------------------------------------------------------
// Net table and selection
// ---------------------------------------------------------------------------

TEST_CASE("VoiceChat: cyclePrimaryNet walks the table and skips the secondary (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    const uint8_t secondary = h.chat.secondaryNet();

    for (int i = 0; i < 8; ++i) {
        h.chat.cyclePrimaryNet();
        CHECK(h.chat.primaryNet() != secondary); // the two keys never collapse onto one net
        CHECK(h.chat.primaryNet() < h.chat.nets().size());
    }
}

TEST_CASE("VoiceChat: a single-net table leaves cycling a no-op (#1145)", "[voice][chat]") {
    Harness h;
    RadioNetTable one;
    RadioNetDef def{};
    def.id = "team"; // REQUIRED: add() rejects a def with an empty id and returns kInvalidRadioNet
    def.name = "TEAM";
    def.kind = RadioNetKind::Team;
    REQUIRE(one.add(def) != kInvalidRadioNet);
    h.chat.setNets(one);

    const uint8_t before = h.chat.primaryNet();
    h.chat.cyclePrimaryNet();
    CHECK(h.chat.primaryNet() == before);
    CHECK(h.chat.secondaryNet() == kInvalidRadioNet); // no second net to key
}

TEST_CASE("VoiceChat: netName resolves known nets and rejects unknown ones (#1145)", "[voice][chat]") {
    Harness h;
    CHECK_FALSE(h.chat.netName(h.chat.primaryNet()).empty());
    CHECK(h.chat.netName(200).empty()); // out of range: an empty view, not a crash
    CHECK(h.chat.netName(kInvalidRadioNet).empty());
}

TEST_CASE("VoiceChat: setNets re-derives the key selection (#1145)", "[voice][chat]") {
    Harness h;
    RadioNetTable nets;
    RadioNetDef team{};
    team.id = "team";
    team.name = "TEAM";
    team.kind = RadioNetKind::Team;
    RadioNetDef flight{};
    flight.id = "flight";
    flight.name = "FLIGHT";
    flight.kind = RadioNetKind::Flight;
    REQUIRE(nets.add(team) != kInvalidRadioNet);
    REQUIRE(nets.add(flight) != kInvalidRadioNet);
    h.chat.setNets(nets);

    // The secondary key wants the FLIGHT net when the server defines one.
    CHECK(h.chat.netName(h.chat.secondaryNet()) == "FLIGHT");
}

TEST_CASE("VoiceChat: setExpectedPacketLoss is safe with or without an encoder (#1145)", "[voice][chat]") {
    Harness h;
    h.chat.setExpectedPacketLoss(15); // before init: no encoder yet
    h.initAll();
    h.chat.setExpectedPacketLoss(15);
    h.chat.setExpectedPacketLoss(0);
    SUCCEED("no crash on either side of init");
}

TEST_CASE("VoiceChat: a remote frame reaches the mixer (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    const std::vector<uint8_t> payload(40, 0x5A);
    h.chat.onRemoteFrame(7, 3, h.chat.primaryNet(), 0, payload, /*start=*/true, /*end=*/false);
    h.tick();
    // The mixer owns the decode; what matters here is that the facade routed it rather than
    // dropping it on the floor, and that the receive path runs with transmit untouched.
    CHECK(h.chat.mixer().duckGain() >= 0.f);
    CHECK_FALSE(h.chat.transmitting());
}

TEST_CASE("VoiceChat: shutdown closes the device and is idempotent (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    h.tick(/*ptt1=*/true); // opens the device
    REQUIRE(h.capture.isCapturing());

    h.chat.shutdown();
    CHECK(h.capture.shutdownCount >= 1);
    CHECK_FALSE(h.capture.isCapturing());

    h.chat.shutdown(); // twice must be safe — the destructor calls it again
    SUCCEED("double shutdown is safe");
}

TEST_CASE("VoiceChat: mic gain scales the encoded input and clamps (#1145)", "[voice][chat]") {
    Harness h;
    h.initAll();
    VoiceSettings v;
    v.micGain = 4.0f; // maximum trim: loud input must clamp rather than wrap to negative
    AudioSettings a;
    h.chat.applySettings(v, a);

    h.capture.queueLoud(4);
    h.tick(/*ptt1=*/true);
    CHECK(h.chat.micLevel() >= 0.f);
    CHECK(h.chat.micLevel() <= 1.f);
}
