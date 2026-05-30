// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/MusicManager.h"

#include "ILogger.h"
#include "audio/ogg_impl.h"
#include "content/AssetManager.h"

#include <algorithm>
#include <cstring>

static const char* gameStateName(GameState s) {
    switch (s) {
    case GameState::Menu:
        return "Menu";
    case GameState::FlightPatrol:
        return "FlightPatrol";
    case GameState::FlightCombat:
        return "FlightCombat";
    case GameState::MissionSuccess:
        return "MissionSuccess";
    case GameState::Debrief:
        return "Debrief";
    }
    return "";
}

bool MusicManager::init(IAudio* audio, AssetManager* assets, ILogger* logger) {
    m_audio = audio;
    m_assets = assets;
    m_logger = logger;

    m_primary.source = audio->createSource();
    m_fade.source = audio->createSource();
    if (!m_primary.source || !m_fade.source) {
        logger->log(LogLevel::Warn, __FILE__, __LINE__, "music: failed to create streaming sources");
        return false;
    }

    for (int i = 0; i < kNumStreamBuffers; ++i) {
        m_primary.bufs[i] = audio->allocStreamBuffer();
        m_fade.bufs[i] = audio->allocStreamBuffer();
        if (!m_primary.bufs[i] || !m_fade.bufs[i]) {
            logger->log(LogLevel::Warn, __FILE__, __LINE__, "music: failed to allocate streaming buffers");
            return false;
        }
    }

    for (AudioSourceId src : {m_primary.source, m_fade.source}) {
        audio->setSourceRelative(src, true);
        audio->setRolloffFactor(src, 0.0f);
        audio->setPosition(src, 0.0f, 0.0f, 0.0f);
        audio->setGain(src, 0.0f);
    }
    return true;
}

void MusicManager::loadPlaylist(const PlaylistData& playlist) {
    m_playlist = playlist;
}

// Fills one streaming buffer and queues it onto the source. Returns false at EOF.
static bool fillAndQueue(IAudio* audio, AudioSourceId src, AudioBufferId buf, OggStreamImpl* stream, int sampleRate,
                         int channels) {
    // Stack buffer: kDecodeChunkSamples samples * up to 2 channels * 2 bytes each = 32 KB max.
    int16_t pcm[MusicManager::kDecodeChunkSamples * 2];
    int decoded = ogg_stream_read(stream, pcm, MusicManager::kDecodeChunkSamples);
    if (decoded <= 0)
        return false;
    audio->queueBuffer(src, buf, pcm, static_cast<std::size_t>(decoded) * channels * sizeof(int16_t), sampleRate,
                       channels);
    return true;
}

void MusicManager::openSlot(StreamSlot& slot, const std::string& assetName) {
    stopSlot(slot);

    auto audioAsset = m_assets->loadAudio(assetName.c_str());
    if (!audioAsset || audioAsset->bytes.empty()) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__,
                      (std::string("music: track not found: ") + assetName).c_str());
        return;
    }

    slot.oggBytes = audioAsset->bytes;
    slot.oggStream = ogg_stream_open(slot.oggBytes.data(), static_cast<int>(slot.oggBytes.size()));
    if (!slot.oggStream) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__,
                      (std::string("music: failed to open OGG: ") + assetName).c_str());
        slot.oggBytes.clear();
        return;
    }

    auto* s = static_cast<OggStreamImpl*>(slot.oggStream);
    slot.sampleRate = ogg_stream_sample_rate(s);
    slot.channels = ogg_stream_channels(s);

    // Prime: fill all kNumStreamBuffers buffers and queue them before starting playback.
    int primed = 0;
    for (int i = 0; i < kNumStreamBuffers; ++i) {
        if (!fillAndQueue(m_audio, slot.source, slot.bufs[i], s, slot.sampleRate, slot.channels))
            break;
        ++primed;
    }

    if (primed == 0) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__, (std::string("music: empty OGG: ") + assetName).c_str());
        ogg_stream_close(s);
        slot.oggStream = nullptr;
        slot.oggBytes.clear();
        return;
    }

    slot.active = true;
    m_audio->resume(slot.source); // alSourcePlay — starts consuming queued buffers
}

void MusicManager::refillSlot(StreamSlot& slot) {
    if (!slot.active || !slot.oggStream)
        return;

    int processed = m_audio->processedBufferCount(slot.source);
    if (processed <= 0)
        return;

    AudioBufferId unqueued[kNumStreamBuffers]{};
    m_audio->unqueueProcessed(slot.source, unqueued, processed);

    auto* s = static_cast<OggStreamImpl*>(slot.oggStream);
    for (int i = 0; i < processed; ++i) {
        if (!unqueued[i])
            continue;
        if (!fillAndQueue(m_audio, slot.source, unqueued[i], s, slot.sampleRate, slot.channels)) {
            // EOF — mark inactive; update() will advance track or loop.
            slot.active = false;
            return;
        }
    }

    // If the source stopped due to buffer starvation, restart it.
    if (!m_audio->isPlaying(slot.source))
        m_audio->resume(slot.source);
}

void MusicManager::stopSlot(StreamSlot& slot) {
    if (slot.source)
        m_audio->detachBuffers(slot.source); // stops + detaches all queued AL buffers
    if (slot.oggStream) {
        ogg_stream_close(static_cast<OggStreamImpl*>(slot.oggStream));
        slot.oggStream = nullptr;
    }
    slot.oggBytes.clear();
    slot.active = false;
    slot.gain = 0.0f;
}

const PlaylistState* MusicManager::currentPlaylistState() const {
    return m_playlist.findState(m_stateId);
}

void MusicManager::setState(GameState state) {
    const char* newId = gameStateName(state);
    if (m_stateId == newId && m_primary.active)
        return;

    const PlaylistState* ps = m_playlist.findState(newId);
    if (!ps || ps->tracks.empty()) {
        stopSlot(m_primary);
        m_state = state;
        m_stateId = newId;
        m_trackIndex = 0;
        return;
    }

    // Move primary → fade for crossfade.
    if (m_primary.active) {
        stopSlot(m_fade);
        std::swap(m_primary, m_fade);
        m_fade.gain = (m_fade.gain > 0.0f) ? m_fade.gain : 1.0f;
        m_crossfadeElapsed = 0.0f;
        m_crossfading = true;
    }

    m_state = state;
    m_stateId = newId;
    m_trackIndex = 0;
    m_primary.gain = 0.0f;
    openSlot(m_primary, ps->tracks[0]);
}

void MusicManager::update(float dt, float masterVolume, float musicVolume) {
    if (!m_audio)
        return;

    // Advance crossfade.
    if (m_crossfading) {
        float dur = (m_playlist.crossfadeDuration > 0.0f) ? m_playlist.crossfadeDuration : 3.0f;
        m_crossfadeElapsed += dt;
        float t = std::min(m_crossfadeElapsed / dur, 1.0f);
        m_primary.gain = t;
        m_fade.gain = 1.0f - t;
        if (t >= 1.0f) {
            stopSlot(m_fade);
            m_primary.gain = 1.0f;
            m_crossfading = false;
        }
    } else if (m_primary.active) {
        m_primary.gain = 1.0f;
    }

    // Apply gains.
    float base = masterVolume * musicVolume;
    if (m_primary.source)
        m_audio->setGain(m_primary.source, base * m_primary.gain);
    if (m_fade.source)
        m_audio->setGain(m_fade.source, base * m_fade.gain);

    // Refill streaming buffers.
    refillSlot(m_primary);
    refillSlot(m_fade);

    // Handle track end (EOF flagged by refillSlot).
    if (!m_primary.active && !m_stateId.empty()) {
        const PlaylistState* ps = currentPlaylistState();
        if (ps && !ps->tracks.empty() && ps->loop) {
            int n = static_cast<int>(ps->tracks.size());
            m_trackIndex = (m_trackIndex + 1) % n;
            openSlot(m_primary, ps->tracks[m_trackIndex]);
        }
    }
}

void MusicManager::shutdown() {
    stopSlot(m_primary);
    stopSlot(m_fade);

    if (m_audio) {
        for (int i = 0; i < kNumStreamBuffers; ++i) {
            if (m_primary.bufs[i])
                m_audio->freeBuffer(m_primary.bufs[i]);
            if (m_fade.bufs[i])
                m_audio->freeBuffer(m_fade.bufs[i]);
        }
        if (m_primary.source)
            m_audio->destroySource(m_primary.source);
        if (m_fade.source)
            m_audio->destroySource(m_fade.source);
    }
    m_audio = nullptr;
}
