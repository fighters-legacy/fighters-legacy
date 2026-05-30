// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"
#include "audio/PlaylistLoader.h"
#include "loop/GameState.h"

#include <cstdint>
#include <string>
#include <vector>

class AssetManager;
class ILogger;

// Streams OGG music tracks via IAudio's streaming buffer API.
// Manages two sources (primary + crossfade) and decodes OGG in kDecodeChunkSamples
// chunks each frame so long tracks never load fully into memory.
//
// Threading: all methods must be called from the main thread.
class MusicManager {
  public:
    // Samples decoded per streaming refill call (~93 ms at 44100 Hz).
    static constexpr int kDecodeChunkSamples = 4096;
    // Number of streaming buffers per source (rolling refill window).
    static constexpr int kNumStreamBuffers = 3;

    bool init(IAudio* audio, AssetManager* assets, ILogger* logger);

    // Loads playlist data and uploads nothing yet — tracks are opened on demand.
    void loadPlaylist(const PlaylistData& playlist);

    // Transitions music to the state matching GameState. No-op if already there.
    void setState(GameState state);

    // Advances streaming decode and crossfade envelope. Call once per frame.
    void update(float dt, float masterVolume, float musicVolume);

    // Stops all music and frees OpenAL resources.
    void shutdown();

  private:
    struct StreamSlot {
        AudioSourceId source{0};
        AudioBufferId bufs[kNumStreamBuffers]{};
        void* oggStream{nullptr};      // OggStreamImpl* from ogg_impl.h (void* to avoid header dep)
        std::vector<uint8_t> oggBytes; // raw OGG bytes; kept alive for the stream's lifetime
        float gain{0.0f};              // current gain envelope [0, 1]
        int sampleRate{0};
        int channels{0};
        bool active{false};

        bool isValid() const {
            return source != 0 && active;
        }
    };

    void openSlot(StreamSlot& slot, const std::string& assetName);
    void refillSlot(StreamSlot& slot);
    void stopSlot(StreamSlot& slot);
    void applyGains(float masterVolume, float musicVolume);

    const PlaylistState* currentPlaylistState() const;

    IAudio* m_audio{nullptr};
    AssetManager* m_assets{nullptr};
    ILogger* m_logger{nullptr};

    PlaylistData m_playlist;
    GameState m_state{GameState::Menu};
    std::string m_stateId;
    int m_trackIndex{0};

    StreamSlot m_primary;
    StreamSlot m_fade;

    float m_crossfadeElapsed{0.0f};
    bool m_crossfading{false};
};
