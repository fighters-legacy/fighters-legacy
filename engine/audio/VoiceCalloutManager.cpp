// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/VoiceCalloutManager.h"

#include "ILogger.h"
#include "audio/OggDecoder.h"
#include "audio/SubtitleQueue.h"
#include "content/AssetManager.h"
#include "i18n/Localization.h"

bool VoiceCalloutManager::init(IAudio* audio, AssetManager* assets, SubtitleQueue* subtitles, Localization* i18n,
                               ILogger* logger, IAudioSynthesizer* synth) {
    m_audio = audio;
    m_assets = assets;
    m_subtitles = subtitles;
    m_i18n = i18n;
    m_logger = logger;
    m_synth = synth;

    for (int i = 0; i < kMaxSfxSources; ++i) {
        m_sources[i] = audio->createSource();
        if (!m_sources[i]) {
            logger->log(LogLevel::Warn, __FILE__, __LINE__, "voice callout: failed to create SFX source");
        } else {
            audio->setSourceRelative(m_sources[i], true);
            audio->setRolloffFactor(m_sources[i], 0.0f);
            audio->setPosition(m_sources[i], 0.0f, 0.0f, 0.0f);
        }
    }
    return true;
}

AudioBufferId VoiceCalloutManager::getOrUploadBuffer(const char* assetName) {
    auto it = m_bufferCache.find(assetName);
    if (it != m_bufferCache.end())
        return it->second;

    auto asset = m_assets->loadAudio(assetName);
    if (!asset || asset->bytes.empty()) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__,
                      (std::string("voice callout: audio not found: ") + assetName).c_str());
        return 0;
    }

    DecodedPcm pcm = decodeOgg(asset->bytes);
    if (!pcm.valid()) {
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__,
                      (std::string("voice callout: OGG decode failed: ") + assetName).c_str());
        return 0;
    }

    AudioBufferId id =
        m_audio->uploadBuffer(pcm.samples.data(), pcm.samples.size() * sizeof(int16_t), pcm.sampleRate, pcm.channels);

    if (id)
        m_bufferCache.emplace(assetName, id);
    return id;
}

void VoiceCalloutManager::play(const VoiceCallout& callout, const AudioSettings& settings) {
    // Resolve subtitle text first (needed for both TTS and subtitle display).
    std::string subtitleText;
    if (callout.subtitleKey && m_i18n) {
        const char* loc = m_i18n->get(callout.subtitleKey);
        if (loc)
            subtitleText = loc;
    }

    // Resolve audio buffer (TTS > OGG asset).
    AudioBufferId bufId = 0;

    if (!subtitleText.empty() && m_synth) {
        SynthesisedAudio synth;
        if (m_synth->synthesise(subtitleText, synth) && synth.valid()) {
            bufId = m_audio->uploadBuffer(synth.samples.data(), synth.samples.size() * sizeof(int16_t),
                                          synth.sampleRate, synth.channels);
            // TTS output is not cached — each call may produce different audio.
        }
    }

    if (!bufId && callout.audioAsset)
        bufId = getOrUploadBuffer(callout.audioAsset);

    // Play audio on the next round-robin source.
    if (bufId) {
        AudioSourceId src = m_sources[m_nextSource % kMaxSfxSources];
        m_nextSource = (m_nextSource + 1) % kMaxSfxSources;
        if (src) {
            float gain = settings.masterVolume * settings.voiceChatVolume;
            m_audio->setGain(src, gain);
            m_audio->play(src, bufId);
        }
    }

    // Push subtitle regardless of whether audio played.
    if (!subtitleText.empty() && m_subtitles && m_subtitles->enabled())
        m_subtitles->push(std::move(subtitleText), callout.subtitleDuration);
}

void VoiceCalloutManager::shutdown() {
    if (!m_audio)
        return;
    for (int i = 0; i < kMaxSfxSources; ++i) {
        if (m_sources[i]) {
            m_audio->stop(m_sources[i]);
            m_audio->destroySource(m_sources[i]);
            m_sources[i] = 0;
        }
    }
    for (auto& [name, id] : m_bufferCache)
        m_audio->freeBuffer(id);
    m_bufferCache.clear();
    m_audio = nullptr;
}
