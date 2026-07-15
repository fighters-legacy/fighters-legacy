// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/OggDecoder.h" // DecodedPcm

namespace fl {

// The compiled-in weapon SFX (#631), the audio counterpart to BuiltinGeometry / BuiltinSensors:
// procedurally synthesised so the whole fire path has SOUND with zero content mounted (fl-base-pack
// ships no SFX yet, and CI never opens an audio device — NullAudio no-ops these harmlessly).
//
// DELIBERATELY DETERMINISTIC: the noise source is a fixed integer hash, never rand()/time, so the
// generated PCM is BYTE-STABLE across runs and platforms — a golden test can pin it, and two
// clients synthesise the identical waveform. Mono int16 at kSfxSampleRate.
enum class SfxKind {
    Gunfire,   // a short, sharp cannon burst
    Launch,    // a rocket-motor whoosh
    Release,   // a store leaving the rack: a low mechanical clunk
    Impact,    // a round connecting: a brief metallic tick
    Explosion, // a warhead: a longer noise burst over a low rumble
};

inline constexpr int kSfxSampleRate = 22050;

// Synthesise one builtin SFX. Pure and allocation-bounded; the same kind always yields identical
// PCM. Never fails (returns a valid, non-empty buffer).
[[nodiscard]] DecodedPcm generateBuiltinSfx(SfxKind kind);

} // namespace fl
