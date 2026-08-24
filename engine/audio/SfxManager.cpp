// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/SfxManager.h"

#include "audio/PcmSynth.h" // toI16 / noise / makeMonoPcm / uploadPcm (#1265)

#include "ILogger.h"
#include "audio/OggDecoder.h"
#include "content/AssetManager.h"

namespace fl {

namespace {
const char* kindTag(SfxKind k) {
    switch (k) {
    case SfxKind::Gunfire:
        return "gunfire";
    case SfxKind::Launch:
        return "launch";
    case SfxKind::Release:
        return "release";
    case SfxKind::Impact:
        return "impact";
    case SfxKind::Explosion:
        return "explosion";
    }
    return "?";
}
} // namespace

bool SfxManager::init(IAudio* audio, AssetManager* assets, ILogger* logger) {
    m_audio = audio;
    m_assets = assets;
    m_logger = logger;
    if (!m_audio)
        return true; // headless / CI: a no-op manager, deliberately not an error

    for (int i = 0; i < kMaxVoices; ++i) {
        m_voices[i] = m_audio->createSource();
        if (m_voices[i]) {
            m_audio->setReferenceDistance(m_voices[i], kReferenceDistanceM);
            m_audio->setMaxDistance(m_voices[i], kMaxDistanceM);
            m_audio->setRolloffFactor(m_voices[i], 1.f);
        }
    }
    return true;
}

void SfxManager::registerPreset(const std::string& name, const std::string& packAsset, SfxKind builtin) {
    m_presets[name] = Preset{packAsset, builtin};
}

void SfxManager::updateListener(const glm::vec3& forward, const glm::vec3& up) {
    if (!m_audio)
        return;
    const float origin[3] = {0.f, 0.f, 0.f}; // sources are camera-relative; the listener is the origin
    const float fwd[3] = {forward.x, forward.y, forward.z};
    const float u[3] = {up.x, up.y, up.z};
    m_audio->setListenerTransform(origin, fwd, u);
}

AudioBufferId SfxManager::getOrUploadBuffer(const Preset& preset) {
    // Prefer a pack asset; fall back to the builtin. Keyed distinctly so both can coexist.
    if (!preset.packAsset.empty() && m_assets) {
        const std::string key = "pack:" + preset.packAsset;
        if (auto it = m_bufferCache.find(key); it != m_bufferCache.end())
            return it->second;
        if (auto asset = m_assets->loadAudio(preset.packAsset.c_str()); asset && !asset->bytes.empty()) {
            if (DecodedPcm pcm = decodeOgg(asset->bytes); pcm.valid()) {
                const AudioBufferId id = uploadPcm(*m_audio, pcm);
                if (id) {
                    m_bufferCache.emplace(key, id);
                    return id;
                }
            }
        }
        // Pack asset missing or undecodable: fall through to the builtin.
    }

    const std::string key = std::string("builtin:") + kindTag(preset.builtin);
    if (auto it = m_bufferCache.find(key); it != m_bufferCache.end())
        return it->second;
    const DecodedPcm pcm = generateBuiltinSfx(preset.builtin);
    const AudioBufferId id = uploadPcm(*m_audio, pcm);
    if (id)
        m_bufferCache.emplace(key, id);
    return id;
}

void SfxManager::play(const std::string& preset, const glm::dvec3& worldPos, const glm::dvec3& cameraOrigin,
                      const AudioSettings& settings, float intensity) {
    if (!m_audio)
        return;
    const auto pit = m_presets.find(preset);
    if (pit == m_presets.end())
        return; // unknown preset — silent, forward-compatible
    const AudioBufferId buf = getOrUploadBuffer(pit->second);
    if (!buf)
        return;

    const AudioSourceId src = m_voices[m_nextVoice];
    m_nextVoice = (m_nextVoice + 1) % kMaxVoices; // steal-oldest
    if (!src)
        return;

    // Camera-relative: subtract in double, cast the small offset to float (planet-scale safe).
    const glm::vec3 rel = glm::vec3(worldPos - cameraOrigin);
    m_audio->stop(src); // reclaim the voice if it was still ringing
    m_audio->setPosition(src, rel.x, rel.y, rel.z);
    m_audio->setGain(src, settings.masterVolume * settings.sfxVolume * intensity);
    m_audio->play(src, buf);
}

void SfxManager::shutdown() {
    if (!m_audio)
        return;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (m_voices[i]) {
            m_audio->stop(m_voices[i]);
            m_audio->destroySource(m_voices[i]);
            m_voices[i] = 0;
        }
    }
    for (auto& [key, id] : m_bufferCache)
        m_audio->freeBuffer(id);
    m_bufferCache.clear();
    m_audio = nullptr;
}

void registerBuiltinSfxPresets(SfxManager& sfx) {
    sfx.registerPreset("sfx.gunfire", "sfx/gunfire", SfxKind::Gunfire);
    sfx.registerPreset("sfx.launch", "sfx/launch", SfxKind::Launch);
    sfx.registerPreset("sfx.release", "sfx/release", SfxKind::Release);
    sfx.registerPreset("sfx.impact", "sfx/impact", SfxKind::Impact);
    sfx.registerPreset("sfx.explosion", "sfx/explosion", SfxKind::Explosion);
}

} // namespace fl
