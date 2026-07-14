// SPDX-License-Identifier: GPL-3.0-or-later
//
// ClientEffectRouter (#625): the wire's cosmetic vocabulary mapped to particle presets. The rows
// are code, not data — but the SUBSTANCE behind each preset name is content-overridable, so the
// tests only pin the names and lifecycle, never colors or spawn rates.
#include <catch2/catch_test_macros.hpp>

#include "ClientEffectRouter.h"

#include "ILogger.h"
#include "audio/SfxManager.h"
#include "mock_hal.h" // MockAudio

#include <cstring>
#include <string>
#include <vector>

using namespace fl;

namespace {
struct NullLoggerER : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};
} // namespace

namespace {

ParticleSystem makePs() {
    ParticleSystem ps;
    ParticlePreset p{};
    p.spawnRate = 60.f;
    p.particleLifetime = 0.5f;
    ps.registerPreset("muzzle_flash", p);
    ps.registerPreset("impact_sparks", p);
    ps.registerPreset("explosion", p);
    return ps;
}

EffectEvent ev(EffectType t, glm::dvec3 pos = {1.0, 2.0, 3.0}) {
    EffectEvent e;
    e.type = static_cast<uint8_t>(t);
    e.pos = pos;
    return e;
}

std::vector<std::string> emitterNames(ClientEffectRouter& r, ParticleSystem& ps, float dt = 0.f) {
    std::vector<std::string> names;
    for (const ParticleEmitterState& e : r.buildEmitters(ps, dt))
        names.emplace_back(e.effectName);
    return names;
}

} // namespace

TEST_CASE("EffectRouter: each wire type maps to its preset; unknown types are no-ops", "[effect_router]") {
    ClientEffectRouter r;
    ParticleSystem ps = makePs();

    r.onEffect(ev(EffectType::WeaponFired));
    r.onEffect(ev(EffectType::Impact));
    r.onEffect(ev(EffectType::Detonation));
    EffectEvent junk;
    junk.type = 200; // future vocabulary: skipped, never rejected
    r.onEffect(junk);

    const auto names = emitterNames(r, ps);
    REQUIRE(names.size() == 3u);
    CHECK(names[0] == "muzzle_flash");
    CHECK(names[1] == "impact_sparks");
    CHECK(names[2] == "explosion");
}

TEST_CASE("EffectRouter: effects burn down their ttl and disappear", "[effect_router]") {
    ClientEffectRouter r;
    ParticleSystem ps = makePs();

    r.onEffect(ev(EffectType::WeaponFired)); // 0.08 s muzzle flash
    CHECK(emitterNames(r, ps, 0.05f).size() == 1u);
    CHECK(emitterNames(r, ps, 0.05f).empty()); // 0.10 s in: expired and swap-erased
    CHECK(emitterNames(r, ps, 0.05f).empty()); // stays empty — nothing lingers
}

TEST_CASE("EffectRouter: a preset the ParticleSystem does not know is skipped, not fatal", "[effect_router]") {
    ClientEffectRouter r;
    ParticleSystem empty; // zero-pack client with no presets registered at all
    r.onEffect(ev(EffectType::Detonation));
    CHECK(r.buildEmitters(empty, 0.f).empty());
}

TEST_CASE("EffectRouter: a burst past kMaxActive overwrites the oldest, never grows", "[effect_router]") {
    ClientEffectRouter r;
    ParticleSystem ps = makePs();
    for (int i = 0; i < ClientEffectRouter::kMaxActive + 8; ++i)
        r.onEffect(ev(EffectType::Detonation));
    CHECK(emitterNames(r, ps).size() == static_cast<std::size_t>(ClientEffectRouter::kMaxActive));
}

TEST_CASE("EffectRouter: routeEffectsTlv decodes packed records and drops a trailing partial", "[effect_router]") {
    ClientEffectRouter r;
    ParticleSystem ps = makePs();

    // Two full records + 5 stray bytes (a truncated third — fail closed).
    std::vector<uint8_t> tlv(2 * kEffectRecordBytes + 5, 0u);
    auto write = [&](std::size_t at, EffectType t, float x) {
        uint8_t* p = tlv.data() + at;
        p[0] = static_cast<uint8_t>(t);
        p[1] = 0u;
        const uint32_t src = 7u, tgt = 9u;
        std::memcpy(p + 2, &src, 4);
        std::memcpy(p + 6, &tgt, 4);
        const float pos[3] = {x, 100.f, -3.f};
        std::memcpy(p + 10, pos, 12);
    };
    write(0, EffectType::WeaponFired, 10.f);
    write(kEffectRecordBytes, EffectType::Impact, 20.f);
    tlv[2 * kEffectRecordBytes] = static_cast<uint8_t>(EffectType::Detonation); // the partial

    routeEffectsTlv(r, tlv.data(), tlv.size());
    const auto emitters = r.buildEmitters(ps, 0.f);
    REQUIRE(emitters.size() == 2u);
    CHECK(std::string(emitters[0].effectName) == "muzzle_flash");
    CHECK(emitters[0].position.x == 10.f);
    CHECK(std::string(emitters[1].effectName) == "impact_sparks");
    CHECK(emitters[1].position.x == 20.f);
}

// ---------------------------------------------------------------------------
// Audio wiring (#631)
// ---------------------------------------------------------------------------

TEST_CASE("EffectRouter: wired to an SfxManager, effects play their SFX", "[effect_router][sfx]") {
    NullLoggerER log;
    MockAudio audio;
    SfxManager sfx;
    sfx.init(&audio, nullptr, &log);
    sfx.registerPreset("sfx.gunfire", "", SfxKind::Gunfire);
    sfx.registerPreset("sfx.explosion", "", SfxKind::Explosion);

    AudioSettings settings;
    ClientEffectRouter r;
    r.setSfx(&sfx, &settings);

    r.onEffect(ev(EffectType::WeaponFired));
    r.onEffect(ev(EffectType::Detonation));
    CHECK(audio.playCount == 2); // gunfire + explosion

    // An effect with no SFX mapping (none currently) or an unregistered preset stays silent; here
    // the router still routes the particle side, so buildEmitters is unaffected.
    ParticleSystem ps = makePs();
    CHECK(emitterNames(r, ps).size() == 2u);
}

TEST_CASE("EffectRouter: with no SfxManager wired, it is exactly the particle-only router", "[effect_router][sfx]") {
    ClientEffectRouter r; // no setSfx
    r.onEffect(ev(EffectType::WeaponFired));
    ParticleSystem ps = makePs();
    CHECK(emitterNames(r, ps).size() == 1u); // particles still work; no audio, no crash
}
