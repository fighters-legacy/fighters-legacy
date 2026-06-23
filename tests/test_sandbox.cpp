// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "mock_hal.h"
#include "sandbox/SandboxInspector.h"

using namespace fl;

TEST_CASE("SandboxInspector constructor uploads one buffer and creates one source", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);

    REQUIRE(audio.uploadCount == 1);
    REQUIRE(audio.createCount == 1);
}

TEST_CASE("SandboxInspector constructor logs ready message", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);

    REQUIRE(logger.hasMessage(LogLevel::Info, "sandbox inspector ready"));
}

TEST_CASE("SandboxInspector update logs entity inspector stub on frame 1", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);
    inspector.update();

    REQUIRE(logger.hasMessage(LogLevel::Info, "entity inspector"));
}

TEST_CASE("SandboxInspector update logs frame stats every 300 frames", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);
    for (int i = 0; i < 300; ++i)
        inspector.update();

    REQUIRE(logger.hasMessage(LogLevel::Info, "sandbox: frame 300"));
}

TEST_CASE("SandboxInspector T key toggles audio tone on then off", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);

    // First T press — should play.
    input.justPressed = {Key::T};
    inspector.update();
    REQUIRE(audio.playCount == 1);
    REQUIRE(audio.stopCount == 0);

    // Second T press — should stop.
    input.justPressed = {Key::T};
    inspector.update();
    REQUIRE(audio.playCount == 1);
    REQUIRE(audio.stopCount == 1);
}

TEST_CASE("SandboxInspector Escape returns false", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);

    input.justPressed = {Key::Escape};
    REQUIRE(inspector.update() == false);
}

TEST_CASE("SandboxInspector returns true on normal frame", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);

    REQUIRE(inspector.update() == true);
}

TEST_CASE("SandboxInspector G key logs game master stub", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    SandboxInspector inspector(audio, input, logger);

    input.justPressed = {Key::G};
    inspector.update();

    REQUIRE(logger.hasMessage(LogLevel::Info, "game master"));
}

TEST_CASE("SandboxInspector logs gamepad axis above threshold", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    input.gamepadCount = 1;
    input.axisValues[{0, GamepadAxis::LeftX}] = 0.8f;

    SandboxInspector inspector(audio, input, logger);
    inspector.update();

    REQUIRE(logger.hasMessage(LogLevel::Info, "gamepad 0 axis"));
}

TEST_CASE("SandboxInspector destructor stops and frees audio resources", "[sandbox]") {
    MockAudio audio;
    MockInput input;
    MockLogger logger;

    {
        SandboxInspector inspector(audio, input, logger);
        // Play the tone so we can verify stop is called on destroy.
        input.justPressed = {Key::T};
        inspector.update();
        REQUIRE(audio.playCount == 1);
    }

    // Destructor must have stopped the source.
    REQUIRE(audio.stopCount >= 1);
}
