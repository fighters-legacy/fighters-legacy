// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/VoiceCalloutManager.h"

#include "ILogger.h"
#include "audio/OggDecoder.h"
#include "audio/SubtitleQueue.h"
#include "content/AssetManager.h"
#include "i18n/Localization.h"

namespace fl {

bool VoiceCalloutManager::init(IAudio* audio, AssetManager* assets, SubtitleQueue* subtitles, Localization* i18n,
                               ILogger* logger, IAudioSynthesizer* synth) {
    m_audio = audio;
    m_assets = assets;
    m_subtitles = subtitles;
    m_i18n = i18n;
    m_logger = logger;
    m_synth = synth;

    // Null audio device (headless / no device): keep the subtitle path live but create no sources.
    // playText/play then push subtitles and skip audio (docs/ai-architecture.md degradation).
    if (!audio)
        return false;

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

namespace {
// click + radio-filtered speech + squelch tail, in one buffer. One buffer rather than three queued
// on a streaming source because a callout is a one-shot: the SFX source pool plays a buffer, it does
// not stream, and splitting the cues across three round-robin sources would let a busy moment
// interleave someone else's callout between a click and the words it announced.
DecodedPcm applyRadioTreatment(const DecodedPcm& in, const RadioProfile& profile) {
    RadioFilter filter;
    filter.configure(profile, in.sampleRate);
    if (!filter.configured())
        return in;

    const std::vector<int16_t> click = radioClickPcm(in.sampleRate);
    const std::vector<int16_t> squelch = radioSquelchPcm(in.sampleRate);
    // The cue generators are mono; interleave them across the source's channel count so a stereo
    // pack asset does not come out half-speed.
    const int ch = in.channels > 0 ? in.channels : 1;

    DecodedPcm out;
    out.sampleRate = in.sampleRate;
    out.channels = in.channels;
    out.samples.reserve(in.samples.size() + (click.size() + squelch.size()) * static_cast<std::size_t>(ch));

    auto appendMono = [&](const std::vector<int16_t>& mono) {
        for (const int16_t s : mono)
            for (int c = 0; c < ch; ++c)
                out.samples.push_back(s);
    };
    appendMono(click);
    const std::size_t speechStart = out.samples.size();
    out.samples.insert(out.samples.end(), in.samples.begin(), in.samples.end());
    filter.process(std::span<int16_t>(out.samples.data() + speechStart, in.samples.size()));
    appendMono(squelch);
    return out;
}
} // namespace

void VoiceCalloutManager::playText(std::string_view text, const char* audioAsset, float subtitleDuration,
                                   const AudioSettings& settings, const RadioProfile* radio, float netGain) {
    std::string subtitleText(text);

    // Audio (TTS > OGG asset), only if an audio device is up. A null/empty asset with no synth = a
    // text-only line, which is the correct degradation with no content pack.
    AudioBufferId bufId = 0;
    if (m_audio) {
        if (!subtitleText.empty() && m_synth) {
            SynthesisedAudio synth;
            if (m_synth->synthesise(subtitleText, synth) && synth.valid()) {
                DecodedPcm pcm;
                pcm.samples = std::move(synth.samples);
                pcm.sampleRate = synth.sampleRate;
                pcm.channels = synth.channels;
                if (radio)
                    pcm = applyRadioTreatment(pcm, *radio);
                bufId = m_audio->uploadBuffer(pcm.samples.data(), pcm.samples.size() * sizeof(int16_t), pcm.sampleRate,
                                              pcm.channels);
                // TTS output is not cached — each call may produce different audio.
            }
        }
        if (!bufId && audioAsset && audioAsset[0] != '\0' && m_assets)
            bufId = radio ? getOrUploadRadioBuffer(audioAsset, *radio) : getOrUploadBuffer(audioAsset);
        if (bufId) {
            AudioSourceId src = m_sources[m_nextSource % kMaxSfxSources];
            m_nextSource = (m_nextSource + 1) % kMaxSfxSources;
            if (src) {
                m_audio->setGain(src, settings.masterVolume * settings.voiceChatVolume * netGain);
                m_audio->play(src, bufId);
            }
        }
    }

    if (!subtitleText.empty() && m_subtitles && m_subtitles->enabled())
        m_subtitles->push(std::move(subtitleText), subtitleDuration);
}

AudioBufferId VoiceCalloutManager::getOrUploadRadioBuffer(const char* assetName, const RadioProfile& profile) {
    // A radio-treated asset is a DIFFERENT buffer from the dry one, so it needs its own cache key —
    // otherwise the first caller's choice would decide how the line sounds for everyone after it.
    const std::string key = std::string("radio:") + assetName;
    if (const auto it = m_bufferCache.find(key); it != m_bufferCache.end())
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
    pcm = applyRadioTreatment(pcm, profile);
    const AudioBufferId id =
        m_audio->uploadBuffer(pcm.samples.data(), pcm.samples.size() * sizeof(int16_t), pcm.sampleRate, pcm.channels);
    if (id)
        m_bufferCache.emplace(key, id);
    return id;
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

} // namespace fl
