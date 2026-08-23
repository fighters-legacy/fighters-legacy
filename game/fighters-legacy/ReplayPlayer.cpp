// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReplayPlayer.h"

#include "net/BitStream.h"
#include "net/RenderEntryFromQuant.h"
#include "net/SnapshotScheduler.h" // kSnapshotRetentionTicks — the same retention window the live client uses

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fl {

bool ReplayPlayer::open(const std::filesystem::path& path) {
    close();
    if (!m_reader.open(path)) {
        m_lastError = m_reader.lastError();
        return false;
    }
    m_open = true;
    m_lastError.clear();
    m_rate = TimeRate::Normal;
    m_resumeRate = TimeRate::Normal;
    m_tickAccumulator = 0.0;
    m_atEnd = false;
    m_havePresented = false;

    // Seed the roster from the section; Join events extend it as playback reaches them.
    for (const ReplayRosterEntry& r : m_reader.sections().roster)
        m_roster[r.participantId] = r.callsign;

    // Decode the first tick so there is a world to show before anything is played.
    resetCaches();
    if (!advanceOneTick()) {
        // An empty recording is valid (see ReplayReader) -- it simply has nothing to show.
        m_atEnd = true;
    }
    return true;
}

void ReplayPlayer::close() {
    m_reader.close();
    m_open = false;
    m_currentTick = 0;
    m_atEnd = false;
    m_havePresented = false;
    m_tickAccumulator = 0.0;
    m_roster.clear();
    resetCaches();
}

void ReplayPlayer::resetCaches() {
    m_cache.clear();
    m_lastEvents.clear();
}

double ReplayPlayer::elapsedSeconds() const noexcept {
    const uint32_t hz = m_reader.header().tickRateHz;
    if (hz == 0 || m_currentTick < m_reader.firstTick())
        return 0.0;
    return static_cast<double>(m_currentTick - m_reader.firstTick()) / static_cast<double>(hz);
}

double ReplayPlayer::progress() const noexcept {
    const uint64_t first = m_reader.firstTick();
    const uint64_t last = m_reader.lastTick();
    if (last <= first)
        return 0.0;
    const double p = static_cast<double>(m_currentTick - first) / static_cast<double>(last - first);
    return std::clamp(p, 0.0, 1.0);
}

void ReplayPlayer::applyTick(const ReplayTick& tick) {
    m_currentTick = tick.tickIndex;
    m_lastEvents = tick.events;

    // A Join carries the joiner's callsign (#643) precisely so a participant who was not in the
    // roster section when recording started still has a name here.
    for (const MatchEvent& e : tick.events) {
        if (e.type == MatchEventType::Join && !e.text.empty())
            m_roster[e.actor] = e.text;
    }

    BitReader r(tick.records.data(), tick.records.size());
    const auto originCount = static_cast<uint32_t>(tick.origins.size() / 3);
    for (uint16_t i = 0; i < tick.recordCount; ++i) {
        QuantEntity qe;
        bool genPresent = false;
        if (!decodeStandaloneRecord(r, qe, tick.origins.data(), originCount, genPresent))
            break; // truncated/malformed: keep what decoded, exactly as the live client does

        // requireGenMatch=false: a replay is a single ordered stream, so there is no recycled-slot
        // race for a generation check to catch. A delta with no baseline means a damaged file.
        (void)m_cache.applyRecord(qe, genPresent, tick.tickIndex, /*requireGenMatch=*/false);
    }

    // Age out anything the recording stopped mentioning. A replay records every entity every tick,
    // so an entity that goes missing is genuinely gone -- but the same retention window the live
    // client uses keeps a damaged file from flickering.
    m_cache.ageOut(tick.tickIndex);
}

bool ReplayPlayer::advanceOneTick() {
    ReplayTick tick;
    if (!m_reader.readNextTick(tick)) {
        m_atEnd = true;
        return false;
    }
    applyTick(tick);
    return true;
}

bool ReplayPlayer::decodeCurrentInto(RenderSnapshot& out) {
    out.entries.clear();
    out.entries.reserve(m_cache.size());
    for (const auto& [idx, c] : m_cache)
        out.entries.push_back(c.re);
    // Ascending entity index: SceneRenderer sorts for draw order anyway, but a stable order keeps
    // per-frame diffs (and any test comparing snapshots) meaningful.
    std::sort(out.entries.begin(), out.entries.end(),
              [](const EntityRenderEntry& a, const EntityRenderEntry& b) { return a.entityIdx < b.entityIdx; });
    out.tickIndex = m_currentTick;
    return true;
}

bool ReplayPlayer::present(RenderSnapshot& out) {
    if (!m_open)
        return false;
    m_havePresented = true;
    return decodeCurrentInto(out);
}

bool ReplayPlayer::update(double dtSeconds, RenderSnapshot& out) {
    if (!m_open)
        return false;

    // The first frame publishes the world as opened, so a paused replay still shows something.
    if (!m_havePresented)
        return present(out);

    const double mult = timeRateMultiplier(m_rate);
    if (mult <= 0.0 || m_atEnd)
        return false; // paused (or finished): the frame is deliberately identical to the last one

    const uint32_t hz = m_reader.header().tickRateHz;
    if (hz == 0)
        return false;

    m_tickAccumulator += dtSeconds * mult * static_cast<double>(hz);
    if (m_tickAccumulator < 1.0)
        return false; // sub-tick frame: nothing new to show

    // Cap the catch-up so a stalled frame (a window drag, a slow disk) does not fast-forward the
    // recording -- the same spiral guard GameLoop applies to the sim.
    constexpr int kMaxTicksPerFrame = 32;
    int steps = static_cast<int>(m_tickAccumulator);
    m_tickAccumulator -= static_cast<double>(steps);
    steps = std::min(steps, kMaxTicksPerFrame);

    bool advanced = false;
    for (int i = 0; i < steps; ++i) {
        if (!advanceOneTick())
            break;
        advanced = true;
    }
    if (!advanced)
        return false;
    return decodeCurrentInto(out);
}

bool ReplayPlayer::seekToTick(uint64_t tick) {
    if (!m_open)
        return false;

    const uint64_t target = std::clamp(tick, m_reader.firstTick(), m_reader.lastTick());

    // Seek to the keyframe at or before the target, then roll forward. The cache is dropped first:
    // its contents describe wherever playback happened to be, and a delta applied to that would
    // paint entities into the wrong places rather than failing visibly.
    if (!m_reader.seekToKeyframeAtOrBefore(target)) {
        m_lastError = m_reader.lastError();
        return false;
    }
    resetCaches();
    m_atEnd = false;

    for (;;) {
        ReplayTick t;
        if (!m_reader.readNextTick(t)) {
            m_atEnd = true;
            break;
        }
        applyTick(t);
        if (t.tickIndex >= target)
            break;
    }
    m_tickAccumulator = 0.0;
    return true;
}

bool ReplayPlayer::seekBySeconds(double deltaSeconds) {
    if (!m_open)
        return false;
    const uint32_t hz = m_reader.header().tickRateHz;
    if (hz == 0)
        return false;

    const double deltaTicks = deltaSeconds * static_cast<double>(hz);
    const auto cur = static_cast<double>(m_currentTick);
    const double target = std::max(0.0, cur + deltaTicks);
    return seekToTick(static_cast<uint64_t>(target));
}

bool ReplayPlayer::seekToFraction(double fraction) {
    if (!m_open)
        return false;
    const uint64_t first = m_reader.firstTick();
    const uint64_t last = m_reader.lastTick();
    if (last <= first)
        return seekToTick(first);
    const double f = std::clamp(fraction, 0.0, 1.0);
    const auto span = static_cast<double>(last - first);
    return seekToTick(first + static_cast<uint64_t>(std::llround(f * span)));
}

} // namespace fl
