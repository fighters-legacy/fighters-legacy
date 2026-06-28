// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// IFlightPattern — pluggable synthetic-input profiles for the bot-swarm load tester.
//
// Each synthetic client owns its own pattern instance (so stateful patterns — an RNG
// walk, a future trace cursor — work), created by name via makePattern(). A pattern maps
// (elapsed seconds, client index) to the five control fields a real client would send in
// MsgClientInput. Built-ins are pure/deterministic so they unit-test without I/O.
//
// Extension point: adding a profile (e.g. "trace:<file>" replay, or a weighted mix) is a
// new IFlightPattern subclass + a branch in makePattern — no harness changes.

#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// The control currency a pattern produces; maps directly onto MsgClientInput fields.
struct BotControl {
    float throttle{0.f}; // [0, 1]
    float elevator{0.f}; // [-1, 1] nose-up positive
    float aileron{0.f};  // [-1, 1] right-roll positive
    float rudder{0.f};   // [-1, 1] right-yaw positive
    uint8_t buttons{0};  // bit 0 = weapon, bit 1 = afterburner
};

class IFlightPattern {
  public:
    virtual ~IFlightPattern() = default;
    // t = seconds since the client became active; clientIndex spreads phase across the swarm.
    virtual BotControl sample(double t, uint32_t clientIndex) = 0;
};

namespace detail {
inline float clampUnit(float v) {
    return v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
}
inline float phaseOf(uint32_t clientIndex) {
    return static_cast<float>(clientIndex) * 0.7f;
}
} // namespace detail

// Gentle weaving turn/climb — entities spread out and move (exercises physics + interest mgmt).
class WeavePattern : public IFlightPattern {
  public:
    BotControl sample(double t, uint32_t clientIndex) override {
        const float ph = detail::phaseOf(clientIndex);
        BotControl c;
        c.throttle = 0.7f;
        c.aileron = 0.3f * std::sin(static_cast<float>(t) * 0.5f + ph);
        c.elevator = 0.1f * std::sin(static_cast<float>(t) * 0.3f + ph * 1.3f);
        return c;
    }
};

// Straight-and-level, constant throttle — near-idle movement; stresses baseline/delta snapshots.
class LevelPattern : public IFlightPattern {
  public:
    BotControl sample(double /*t*/, uint32_t /*clientIndex*/) override {
        BotControl c;
        c.throttle = 0.6f;
        return c;
    }
};

// High-rate rolls/pulls + afterburner — maximum entity churn; stresses physics + snapshot size.
class AggressivePattern : public IFlightPattern {
  public:
    BotControl sample(double t, uint32_t clientIndex) override {
        const float ph = detail::phaseOf(clientIndex);
        BotControl c;
        c.throttle = 1.0f;
        c.aileron = detail::clampUnit(std::sin(static_cast<float>(t) * 2.0f + ph));
        c.elevator = 0.8f * std::sin(static_cast<float>(t) * 1.5f + ph);
        c.buttons = 0x02; // afterburner lit
        return c;
    }
};

// No control input — measures pure connection + snapshot overhead.
class IdlePattern : public IFlightPattern {
  public:
    BotControl sample(double /*t*/, uint32_t /*clientIndex*/) override {
        return {};
    }
};

// Seeded per-client random walk for heterogeneity. Stateful (the client owns the instance);
// deterministic for a given seed + call sequence.
class RandomPattern : public IFlightPattern {
  public:
    explicit RandomPattern(uint32_t seed) : m_rng(seed ? seed : 1u) {}
    BotControl sample(double /*t*/, uint32_t /*clientIndex*/) override {
        std::uniform_real_distribution<float> step(-0.05f, 0.05f);
        m_c.aileron = detail::clampUnit(m_c.aileron + step(m_rng));
        m_c.elevator = detail::clampUnit(m_c.elevator + step(m_rng));
        m_c.rudder = detail::clampUnit(m_c.rudder + step(m_rng));
        float th = m_c.throttle + step(m_rng);
        m_c.throttle = th < 0.f ? 0.f : (th > 1.f ? 1.f : th);
        return m_c;
    }

  private:
    std::mt19937 m_rng;
    BotControl m_c{};
};

// Registry: the names a `--pattern` value may take, and the factory.
inline std::vector<std::string> patternNames() {
    return {"weave", "level", "aggressive", "idle", "random"};
}

inline bool isKnownPattern(std::string_view name) {
    for (const auto& n : patternNames())
        if (n == name)
            return true;
    return false;
}

// Creates a fresh pattern instance for one client. `seed` makes stateful patterns reproducible
// per client. Returns nullptr for an unknown name.
inline std::unique_ptr<IFlightPattern> makePattern(std::string_view name, uint32_t seed) {
    if (name == "weave")
        return std::make_unique<WeavePattern>();
    if (name == "level")
        return std::make_unique<LevelPattern>();
    if (name == "aggressive")
        return std::make_unique<AggressivePattern>();
    if (name == "idle")
        return std::make_unique<IdlePattern>();
    if (name == "random")
        return std::make_unique<RandomPattern>(seed);
    return nullptr;
}

} // namespace fl
