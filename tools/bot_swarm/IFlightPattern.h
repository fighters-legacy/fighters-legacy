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

#include <net/InputTraceReader.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
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

// Weaving flight WITH weapons employment (#583): duty-cycled gun fire plus periodic store releases,
// STAGGERED per client so a 128-swarm does not fire in lockstep on one tick. This is the scale-gate
// "weapons" profile's synthetic load — it drives the fire path, hitscan, projectile spawns and the
// SnapshotEffects TLV under the same physics/interest churn as the weave. Deterministic: the fire
// schedule is a function of (t, clientIndex) only, no RNG.
class WeaponsPattern : public IFlightPattern {
  public:
    BotControl sample(double t, uint32_t clientIndex) override {
        const float ph = detail::phaseOf(clientIndex);
        BotControl c;
        c.throttle = 0.8f;
        c.aileron = 0.3f * std::sin(static_cast<float>(t) * 0.5f + ph);
        c.elevator = 0.1f * std::sin(static_cast<float>(t) * 0.3f + ph * 1.3f);

        // Gun: a per-client-phased 50% duty cycle at ~1 Hz. The server rate-limits it, so this
        // produces a steady stream of hitscan resolutions without a per-tick trigger storm.
        const double gunPhase = t + static_cast<double>(clientIndex) * 0.11;
        if (std::fmod(gunPhase, 2.0) < 1.0)
            c.buttons |= 0x01u;

        // Store release: a short rising-edge pulse every ~5 s, its phase spread across the swarm so
        // the projectile spawns arrive smeared over the window rather than all at once. The server
        // edge-detects bit 2, so holding it across the pulse is still one launch.
        const double firePhase = t + static_cast<double>(clientIndex) * 0.037;
        if (std::fmod(firePhase, 5.0) < 0.1)
            c.buttons |= 0x04u;

        return c;
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
    return {"weave", "level", "aggressive", "idle", "random", "weapons"};
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
    if (name == "weapons")
        return std::make_unique<WeaponsPattern>();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Trace replay (#560)
// ---------------------------------------------------------------------------

// Replays a recorded FLIT trace (see engine/net/InputTraceReader.h) as synthetic input. The
// immutable loaded trace is shared across all clients via shared_ptr; each client offsets its
// playback cursor by clientIndex so a swarm does not fly in lockstep, and playback loops at the
// end of the trace. Stateless per call — deterministic for a given (t, clientIndex).
class TracePattern : public IFlightPattern {
  public:
    explicit TracePattern(std::shared_ptr<const InputTrace> trace) : m_trace(std::move(trace)) {}
    BotControl sample(double t, uint32_t clientIndex) override {
        BotControl c;
        if (!m_trace || m_trace->records.empty())
            return c;
        const auto& recs = m_trace->records;
        const uint32_t rate = m_trace->tickRate ? m_trace->tickRate : 60u;
        const auto n = static_cast<long long>(recs.size());
        // wall-time -> record index at the trace's tick rate, phased per client, looping.
        long long idx = static_cast<long long>(t * static_cast<double>(rate)) + static_cast<long long>(clientIndex);
        const std::size_t i = static_cast<std::size_t>(((idx % n) + n) % n);
        const InputTraceRecord& r = recs[i];
        c.throttle = r.throttle;
        c.elevator = r.elevator;
        c.aileron = r.aileron;
        c.rudder = r.rudder;
        c.buttons = static_cast<uint8_t>(r.buttons & 0xFFu);
        return c;
    }

  private:
    std::shared_ptr<const InputTrace> m_trace;
};

// A --pattern value of the form "trace:<file>" selects trace replay. isKnownPattern covers only
// the built-in registry names (so it also validates --pattern-mix entries); trace specs are
// validated separately because they carry a filesystem path the harness loads once at startup.
inline bool isTracePattern(std::string_view name) {
    constexpr std::string_view kPrefix = "trace:";
    return name.size() > kPrefix.size() && name.substr(0, kPrefix.size()) == kPrefix;
}
inline std::string_view tracePatternPath(std::string_view name) {
    return isTracePattern(name) ? name.substr(std::string_view("trace:").size()) : std::string_view{};
}

// ---------------------------------------------------------------------------
// Weighted pattern mix (#560)
// ---------------------------------------------------------------------------

struct PatternMixEntry {
    std::string name;
    int weight{0};
};

// Parses "weave:80,aggressive:15,idle:5" into weighted entries. Weights must be positive
// integers; names must be built-in patterns. Returns false with `err` set on any malformed
// entry, unknown pattern, or non-positive weight.
inline bool parsePatternMix(std::string_view spec, std::vector<PatternMixEntry>& out, std::string& err) {
    out.clear();
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        const std::string_view tok =
            spec.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (tok.empty()) {
            err = "empty entry in pattern mix";
            return false;
        }
        const std::size_t colon = tok.find(':');
        if (colon == std::string_view::npos) {
            err = "pattern mix entry needs name:weight";
            return false;
        }
        std::string name(tok.substr(0, colon));
        const std::string wstr(tok.substr(colon + 1));
        if (!isKnownPattern(name)) {
            err = "unknown pattern in mix: " + name;
            return false;
        }
        char* end = nullptr;
        const long w = std::strtol(wstr.c_str(), &end, 10);
        if (end == wstr.c_str() || *end != '\0' || w <= 0) {
            err = "pattern mix weight must be a positive integer: " + wstr;
            return false;
        }
        out.push_back({std::move(name), static_cast<int>(w)});
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    if (out.empty()) {
        err = "empty pattern mix";
        return false;
    }
    return true;
}

// Deterministically assigns one client to a mix entry by its index proportion: client i of N maps
// to the cumulative-weight bucket containing floor(i * totalWeight / N). Counts per pattern match
// the weight fractions within rounding, with no RNG (reproducible across runs and platforms).
inline const std::string& assignMixPattern(const std::vector<PatternMixEntry>& mix, uint32_t clientIndex,
                                           int totalClients) {
    long long total = 0;
    for (const auto& e : mix)
        total += e.weight;
    const long long key =
        (totalClients > 0) ? (static_cast<long long>(clientIndex) * total / static_cast<long long>(totalClients)) : 0;
    long long cum = 0;
    for (const auto& e : mix) {
        cum += e.weight;
        if (key < cum)
            return e.name;
    }
    return mix.back().name;
}

// Resolves each synthetic client's flight pattern. Exactly one mode is active: a shared trace
// (trace != null), a weighted mix (mix non-empty), or a single built-in (single). Pure/deterministic
// so the per-client assignment is unit-testable without sockets.
struct SwarmPatternPlan {
    std::string single{"weave"};             // built-in name, used when trace/mix are unset
    std::vector<PatternMixEntry> mix;        // non-empty => weighted mix
    std::shared_ptr<const InputTrace> trace; // non-null => trace replay
    int totalClients{0};                     // denominator for proportional mix assignment

    std::unique_ptr<IFlightPattern> make(uint32_t clientIndex) const {
        if (trace)
            return std::make_unique<TracePattern>(trace);
        if (!mix.empty())
            return makePattern(assignMixPattern(mix, clientIndex, totalClients), clientIndex + 1u);
        return makePattern(single, clientIndex + 1u);
    }
};

} // namespace fl
