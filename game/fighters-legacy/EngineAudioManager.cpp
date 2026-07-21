// SPDX-License-Identifier: GPL-3.0-or-later
#include "EngineAudioManager.h"

#include "ILogger.h"

#include <algorithm>
#include <cmath>

namespace fl {

namespace {

float speedOf(const glm::vec3& v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

bool EngineAudioManager::init(IAudio* audio, ILogger* logger) {
    m_audio = audio;
    m_logger = logger;
    return true; // a null device is a valid, silent configuration
}

void EngineAudioManager::shutdown() {
    if (m_audio) {
        if (m_ownEngine)
            m_audio->destroySource(m_ownEngine);
        if (m_ownWind)
            m_audio->destroySource(m_ownWind);
        for (FlybyVoice& v : m_flyby)
            if (v.src)
                m_audio->destroySource(v.src);
        if (m_engineBuf)
            m_audio->freeBuffer(m_engineBuf);
        if (m_windBuf)
            m_audio->freeBuffer(m_windBuf);
    }
    m_ownEngine = m_ownWind = 0;
    m_engineBuf = m_windBuf = 0;
    m_ownPlaying = false;
    for (FlybyVoice& v : m_flyby)
        v = {};
    m_activeFlyby = 0;
    m_audio = nullptr;
}

AudioBufferId EngineAudioManager::engineBuffer() {
    if (!m_engineBuf && m_audio) {
        const DecodedPcm pcm = generateEngineLoopPcm(EngineLoopKind::Engine);
        m_engineBuf = m_audio->uploadBuffer(pcm.samples.data(), pcm.samples.size() * sizeof(int16_t), pcm.sampleRate,
                                            pcm.channels);
    }
    return m_engineBuf;
}

AudioBufferId EngineAudioManager::windBuffer() {
    if (!m_windBuf && m_audio) {
        const DecodedPcm pcm = generateEngineLoopPcm(EngineLoopKind::Wind);
        m_windBuf = m_audio->uploadBuffer(pcm.samples.data(), pcm.samples.size() * sizeof(int16_t), pcm.sampleRate,
                                          pcm.channels);
    }
    return m_windBuf;
}

void EngineAudioManager::ensureOwnSources() {
    if (!m_audio)
        return;
    const AudioBufferId eng = engineBuffer();
    const AudioBufferId wind = windBuffer();
    if (!m_ownEngine && eng) {
        m_ownEngine = m_audio->createSource();
        if (m_ownEngine) {
            m_audio->setSourceRelative(m_ownEngine, true); // head-locked: your own jet, no distance falloff
            m_audio->setRolloffFactor(m_ownEngine, 0.f);
            m_audio->setPosition(m_ownEngine, 0.f, 0.f, 0.f);
            m_audio->setLooping(m_ownEngine, true);
        }
    }
    if (!m_ownWind && wind) {
        m_ownWind = m_audio->createSource();
        if (m_ownWind) {
            m_audio->setSourceRelative(m_ownWind, true);
            m_audio->setRolloffFactor(m_ownWind, 0.f);
            m_audio->setPosition(m_ownWind, 0.f, 0.f, 0.f);
            m_audio->setLooping(m_ownWind, true);
        }
    }
}

void EngineAudioManager::silenceOwn() {
    if (m_ownPlaying && m_audio) {
        if (m_ownEngine)
            m_audio->stop(m_ownEngine);
        if (m_ownWind)
            m_audio->stop(m_ownWind);
    }
    m_ownPlaying = false;
}

void EngineAudioManager::driveOwn(const EntityRenderEntry& own, const AudioSettings& settings) {
    ensureOwnSources();
    if (!m_ownEngine)
        return;

    const float vol = settings.masterVolume * settings.sfxVolume;
    const float throttle01 = static_cast<float>(own.throttle) / 100.f;
    const float airspeed = speedOf(own.velocity);

    const EngineToneParams eng = engineTone(throttle01, airspeed, own.abEngaged);
    m_audio->setPitch(m_ownEngine, eng.pitch);
    m_audio->setGain(m_ownEngine, eng.gain * vol);
    // Pin the source velocity to the listener's own velocity so there is NO doppler on your own
    // engine (a head-locked source still enters OpenAL's doppler formula via its velocity).
    m_audio->setVelocity(m_ownEngine, own.velocity.x, own.velocity.y, own.velocity.z);

    if (m_ownWind) {
        m_audio->setPitch(m_ownWind, windRushPitch(airspeed));
        m_audio->setGain(m_ownWind, windRushGain(airspeed) * vol);
        m_audio->setVelocity(m_ownWind, own.velocity.x, own.velocity.y, own.velocity.z);
    }

    if (!m_ownPlaying) {
        m_audio->play(m_ownEngine, m_engineBuf);
        if (m_ownWind)
            m_audio->play(m_ownWind, m_windBuf);
        m_ownPlaying = true;
    }
}

void EngineAudioManager::driveFlybys(std::span<const EntityRenderEntry> entities, uint32_t ownIdx,
                                     const glm::dvec3& cameraOrigin, const AudioSettings& settings) {
    // Rank candidate air entities by distance; the nearest kMaxFlybyVoices get a positional engine
    // source. A candidate must be an air vehicle (predicate), not the ownship, and within the
    // audible ceiling.
    struct Cand {
        double dist2;
        std::size_t index; // into `entities`
    };
    Cand cands[64];
    int nCand = 0;
    const double maxD2 = static_cast<double>(kFlybyMaxDistanceM) * static_cast<double>(kFlybyMaxDistanceM);
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const EntityRenderEntry& e = entities[i];
        if (e.entityIdx == ownIdx)
            continue;
        if (m_isAir && !m_isAir(e.typeIndex))
            continue;
        const glm::dvec3 rel = e.position - cameraOrigin;
        const double d2 = rel.x * rel.x + rel.y * rel.y + rel.z * rel.z;
        if (d2 > maxD2)
            continue;
        if (nCand < static_cast<int>(std::size(cands)))
            cands[nCand++] = {d2, i};
    }
    const int take = std::min(nCand, kMaxFlybyVoices);
    std::partial_sort(cands, cands + take, cands + nCand,
                      [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });

    // Free any voice whose entity is no longer among the chosen nearest set.
    auto chosen = [&](uint32_t idx) {
        for (int c = 0; c < take; ++c)
            if (entities[cands[c].index].entityIdx == idx)
                return true;
        return false;
    };
    for (FlybyVoice& v : m_flyby) {
        if (v.playing && !chosen(v.entityIdx)) {
            if (v.src && m_audio)
                m_audio->stop(v.src);
            v.playing = false;
            v.entityIdx = kNoEntity;
        }
    }

    const float vol = settings.masterVolume * settings.sfxVolume;
    m_activeFlyby = 0;
    for (int c = 0; c < take; ++c) {
        const EntityRenderEntry& e = entities[cands[c].index];

        // Reuse the voice already tracking this entity (no restart click); else grab a free voice.
        FlybyVoice* voice = nullptr;
        for (FlybyVoice& v : m_flyby)
            if (v.playing && v.entityIdx == e.entityIdx) {
                voice = &v;
                break;
            }
        if (!voice)
            for (FlybyVoice& v : m_flyby)
                if (!v.playing) {
                    voice = &v;
                    break;
                }
        if (!voice)
            continue; // pool exhausted (shouldn't happen: pool size == take cap)

        if (!voice->src && m_audio) {
            voice->src = m_audio->createSource();
            if (voice->src) {
                m_audio->setLooping(voice->src, true);
                m_audio->setReferenceDistance(voice->src, kFlybyReferenceDistanceM);
                m_audio->setMaxDistance(voice->src, kFlybyMaxDistanceM);
                m_audio->setRolloffFactor(voice->src, 1.f);
            }
        }
        if (!voice->src)
            continue;

        const glm::vec3 rel = glm::vec3(e.position - cameraOrigin); // camera-relative (listener at origin)
        const float throttle01 = static_cast<float>(e.throttle) / 100.f;
        const float airspeed = speedOf(e.velocity);
        const EngineToneParams eng = engineTone(throttle01, airspeed, e.abEngaged);

        m_audio->setPosition(voice->src, rel.x, rel.y, rel.z);
        m_audio->setVelocity(voice->src, e.velocity.x, e.velocity.y, e.velocity.z); // → OpenAL doppler
        m_audio->setPitch(voice->src, eng.pitch);
        m_audio->setGain(voice->src, eng.gain * vol);
        if (!voice->playing) {
            voice->entityIdx = e.entityIdx;
            m_audio->play(voice->src, engineBuffer());
            voice->playing = true;
        }
        ++m_activeFlyby;
    }
}

void EngineAudioManager::update(std::span<const EntityRenderEntry> entities, uint32_t ownIdx,
                                const glm::dvec3& cameraOrigin, const AudioSettings& settings) {
    if (!m_audio)
        return;

    const EntityRenderEntry* own = nullptr;
    if (ownIdx != kNoEntity)
        for (const EntityRenderEntry& e : entities)
            if (e.entityIdx == ownIdx) {
                own = &e;
                break;
            }

    if (own)
        driveOwn(*own, settings);
    else
        silenceOwn();

    driveFlybys(entities, ownIdx, cameraOrigin, settings);
}

} // namespace fl
