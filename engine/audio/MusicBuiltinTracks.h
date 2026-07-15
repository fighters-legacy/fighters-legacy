// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/OggDecoder.h" // DecodedPcm
#include "audio/PlaylistLoader.h"

#include <span>
#include <string_view>

// Compiled-in procedural music for the zero-content-pack sandbox (#865), the music counterpart to
// SfxBuiltinSounds: the MusicManager streaming / crossfade / state-machine path must produce SOUND
// with no pack playlist mounted (SFX already had generateBuiltinSfx; music silently no-op'd). A null
// IAudio stays a clean no-op, exactly like the SFX path (CI opens no audio device).
//
// DELIBERATELY DETERMINISTIC: synthesised from fixed note tables + float math, never rand()/time, so
// the PCM is BYTE-STABLE across runs and platforms and a golden test can pin it. Short mono loops.
namespace fl {

enum class MusicMood {
    Menu,   // a slow, calm ambient pad
    Patrol, // a mid-tempo drifting theme
    Combat, // a faster, tense, driving pulse
};

inline constexpr int kMusicSampleRate = 22050;

// Synthesise one builtin music loop. Pure and deterministic; the same mood always yields identical
// PCM. Never fails (returns a valid, non-empty mono buffer).
[[nodiscard]] DecodedPcm generateBuiltinMusic(MusicMood mood);

// Map a builtin music track ASSET NAME ("builtin:music-menu" / "-patrol" / "-combat") to its PCM, or
// an empty DecodedPcm if the name is not a builtin track. MusicManager checks this before AssetManager
// so builtin tracks stream with no pack.
[[nodiscard]] DecodedPcm builtinMusicTrack(std::string_view assetName);

// The three builtin track asset names, for listing / cache seeding.
[[nodiscard]] std::span<const std::string_view> builtinMusicTrackNames() noexcept;

// The default PlaylistData wiring the builtin tracks to game states (Menu/FlightPatrol/FlightCombat/
// MissionSuccess/Debrief). Used when no pack playlist.toml is present; a pack playlist overrides it.
[[nodiscard]] PlaylistData builtinDefaultPlaylist();

} // namespace fl
