// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/MusicManager.h"

#include "ILogger.h"
#include "audio/MusicBuiltinTracks.h"
#include "content/AssetManager.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace fl {

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
    m_rng = std::mt19937{std::random_device{}()};
    return true;
}

void MusicManager::loadPlaylist(const PlaylistData& playlist) {
    m_playlist = playlist;
}

// Fills one streaming buffer and queues it onto the source. Returns false at EOF.
static bool fillAndQueue(IAudio* audio, AudioSourceId src, AudioBufferId buf, OggStream* stream, int sampleRate,
                         int channels) {
    // Stack buffer: kDecodeChunkSamples samples * up to 2 channels * 2 bytes each = 32 KB max.
    int16_t pcm[MusicManager::kDecodeChunkSamples * 2];
    int decoded = readOggSamples(stream, pcm, MusicManager::kDecodeChunkSamples);
    if (decoded <= 0)
        return false;
    audio->queueBuffer(src, buf, pcm, static_cast<std::size_t>(decoded) * channels * sizeof(int16_t), sampleRate,
                       channels);
    return true;
}

// Fills one streaming buffer from a looping PCM buffer (a builtin procedural track) and queues it,
// wrapping at the end so the loop is seamless. Never "ends" — a builtin track loops forever.
static void fillAndQueuePcm(IAudio* audio, AudioSourceId src, AudioBufferId buf, const std::vector<int16_t>& loop,
                            std::size_t& pos, int sampleRate, int channels) {
    int16_t pcm[MusicManager::kDecodeChunkSamples];
    const std::size_t want = static_cast<std::size_t>(MusicManager::kDecodeChunkSamples);
    for (std::size_t i = 0; i < want; ++i) {
        pcm[i] = loop[pos];
        if (++pos >= loop.size())
            pos = 0;
    }
    audio->queueBuffer(src, buf, pcm, want * sizeof(int16_t), sampleRate, channels);
}

void MusicManager::openSlot(StreamSlot& slot, const std::string& assetName) {
    stopSlot(slot);

    // Builtin procedural track (#865): synthesised PCM, looped through the same streaming buffers as an
    // OGG track (crossfade + gain machinery unchanged). Checked BEFORE AssetManager so it needs no pack.
    if (DecodedPcm pcm = builtinMusicTrack(assetName); pcm.valid()) {
        slot.pcmLoop = std::move(pcm.samples);
        slot.sampleRate = pcm.sampleRate;
        slot.channels = pcm.channels;
        slot.pcmPos = 0;
        for (int i = 0; i < kNumStreamBuffers; ++i)
            fillAndQueuePcm(m_audio, slot.source, slot.bufs[i], slot.pcmLoop, slot.pcmPos, slot.sampleRate,
                            slot.channels);
        slot.active = true;
        m_audio->resume(slot.source);
        return;
    }

    auto audioAsset = m_assets->loadAudio(assetName.c_str());
    if (!audioAsset || audioAsset->bytes.empty()) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__,
                      (std::string("music: track not found: ") + assetName).c_str());
        return;
    }

    slot.oggBytes = audioAsset->bytes;
    slot.oggStream.reset(openOggStream(slot.oggBytes));
    if (!slot.oggStream) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__,
                      (std::string("music: failed to open OGG: ") + assetName).c_str());
        slot.oggBytes.clear();
        return;
    }

    const OggStreamInfo info = getOggStreamInfo(slot.oggStream.get());
    slot.sampleRate = info.sampleRate;
    slot.channels = info.channels;

    // Prime: fill all kNumStreamBuffers buffers and queue them before starting playback.
    int primed = 0;
    for (int i = 0; i < kNumStreamBuffers; ++i) {
        if (!fillAndQueue(m_audio, slot.source, slot.bufs[i], slot.oggStream.get(), slot.sampleRate, slot.channels))
            break;
        ++primed;
    }

    if (primed == 0) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__, (std::string("music: empty OGG: ") + assetName).c_str());
        slot.oggStream.reset();
        slot.oggBytes.clear();
        return;
    }

    slot.active = true;
    m_audio->resume(slot.source); // alSourcePlay — starts consuming queued buffers
}

void MusicManager::refillSlot(StreamSlot& slot) {
    const bool builtin = !slot.pcmLoop.empty();
    if (!slot.active || (!slot.oggStream && !builtin))
        return;

    int processed = m_audio->processedBufferCount(slot.source);
    if (processed <= 0)
        return;

    AudioBufferId unqueued[kNumStreamBuffers]{};
    m_audio->unqueueProcessed(slot.source, unqueued, processed);

    for (int i = 0; i < processed; ++i) {
        if (!unqueued[i])
            continue;
        if (builtin) {
            fillAndQueuePcm(m_audio, slot.source, unqueued[i], slot.pcmLoop, slot.pcmPos, slot.sampleRate,
                            slot.channels); // loops forever, never EOF
        } else if (!fillAndQueue(m_audio, slot.source, unqueued[i], slot.oggStream.get(), slot.sampleRate,
                                 slot.channels)) {
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
    slot.oggStream.reset();
    slot.oggBytes.clear();
    slot.pcmLoop.clear();
    slot.pcmPos = 0;
    slot.active = false;
    slot.gain = 0.0f;
}

const PlaylistState* MusicManager::currentPlaylistState() const {
    return m_playlist.findState(m_stateId);
}

void MusicManager::buildTrackOrder(const PlaylistState& ps) {
    m_trackOrder = ps.tracks;
    if (ps.shuffle)
        std::shuffle(m_trackOrder.begin(), m_trackOrder.end(), m_rng);
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
    buildTrackOrder(*ps);
    m_trackIndex = 0;
    m_primary.gain = 0.0f;
    openSlot(m_primary, m_trackOrder[0]);
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
            int n = static_cast<int>(m_trackOrder.size());
            int next = m_trackIndex + 1;
            if (next >= n) {
                if (ps->shuffle)
                    std::shuffle(m_trackOrder.begin(), m_trackOrder.end(), m_rng);
                m_trackIndex = 0;
            } else {
                m_trackIndex = next;
            }
            openSlot(m_primary, m_trackOrder[m_trackIndex]);
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

} // namespace fl
