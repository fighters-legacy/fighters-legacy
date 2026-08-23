// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::TickRate (#1075) and the conversions #1253 folded back into it.
//
// TickRate.h says it is where "where is 60 decided" gets its one answer. It was not: the server
// declared its own kSimTickRateHz, client prediction its own kPredTickDt, and Game.cpp spelled
// tick-to-time out four more times. This pins the accessors those sites now go through -- including
// that a non-60 rate really does propagate, which is the whole point of the field being honest.

#include "net/TickRate.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("the default rate is 60 and the accessors agree with the literals they replaced", "[tick_rate]") {
    constexpr fl::TickRate r = fl::kServerTickRate;
    CHECK(r.hz() == 60);

    // Each of these is bit-identical to the expression it replaced at a call site.
    CHECK(r.dtSeconds() == 1.f / 60.f);         // ClientPrediction's kPredTickDt
    CHECK(r.dtSecondsDouble() == 1.0 / 60.0);   // ServerRuntime's kSimDt
    CHECK(r.msPerTick() == 1000.0f / 60.0f);    // the debug overlay's frame budget
    CHECK(r.ticksToSeconds(90) == 90.0 / 60.0); // Game.cpp's sim-time readout
}

TEST_CASE("the float and double periods are NOT the same number", "[tick_rate]") {
    // This is why dtSecondsDouble exists rather than callers widening dtSeconds(). 1.0f/60 and
    // 1.0/60 differ, so a sim accumulating in double off the float accessor drifts against a sim
    // accumulating off the double one -- a tick's worth of time would depend on which accessor the
    // caller happened to reach for.
    constexpr fl::TickRate r = fl::kServerTickRate;
    CHECK(static_cast<double>(r.dtSeconds()) != r.dtSecondsDouble());
}

TEST_CASE("a non-default rate propagates through every conversion", "[tick_rate]") {
    // The #1075 property: the rate arrives on MsgConnectAck, so a server stepping at 30 Hz must
    // move every derived quantity. A hardcoded 1/60 anywhere silently breaks this.
    constexpr fl::TickRate slow{30};
    CHECK(slow.hz() == 30);
    CHECK(slow.dtSecondsDouble() == 1.0 / 30.0);
    CHECK(slow.dtSecondsDouble() > fl::kServerTickRate.dtSecondsDouble());
    CHECK(slow.msPerTick() > fl::kServerTickRate.msPerTick());
    CHECK(slow.ticksToSeconds(30) == 1.0);
    CHECK(slow.ticksToMs(30) == 1000u);
}

TEST_CASE("ticksToSeconds and ticksToMs agree, and invert msToTicks", "[tick_rate]") {
    constexpr fl::TickRate r = fl::kServerTickRate;
    CHECK(r.ticksToSeconds(120) == 2.0);
    CHECK(r.ticksToMs(120) == 2000u);
    CHECK(r.msToTicks(2000) == 120u);

    // ticksToSeconds is the double form and does NOT truncate, which is the difference from
    // ticksToMs/1000 -- the mission clock needs the fraction.
    CHECK(r.ticksToSeconds(90) == 1.5);
    CHECK(r.ticksToMs(90) == 1500u);
}

TEST_CASE("a zero rate falls back rather than reaching a division by zero", "[tick_rate]") {
    // The rate arrives from the wire and every accessor divides by it. A hostile or broken server
    // sending 0 must leave the client merely wrong about the rate, not dead.
    constexpr fl::TickRate bad{0};
    CHECK(bad.hz() == fl::TickRate::kDefaultHz);
    CHECK(bad.dtSecondsDouble() == 1.0 / 60.0);
    CHECK(bad.ticksToSeconds(60) == 1.0);
}

TEST_CASE("the conversions are usable at compile time", "[tick_rate]") {
    static_assert(fl::kServerTickRate.dtSecondsDouble() == 1.0 / 60.0);
    static_assert(fl::kServerTickRate.ticksToSeconds(60) == 1.0);
    static_assert(fl::TickRate{0}.hz() == 60);
}
