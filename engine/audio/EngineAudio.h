// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/OggDecoder.h" // DecodedPcm

namespace fl {

// The pure core of the continuous engine + aerodynamic sound layers (#959, part of Epic #586): the
// compiled-in looping PCM and the throttle/airspeed → pitch/gain mapping. Lives in engine-audio (no
// render / network deps) so it is unit-testable without a device; the game-layer EngineAudioManager
// (game/fighters-legacy) owns the OpenAL sources that play these buffers, the way ClientEffectRouter
// owns the particle emitters.
//
// Like SfxBuiltinSounds the loops are BYTE-STABLE (a fixed integer harmonic count / a fixed integer
// hash, never rand()/time), so the sim has engine sound with zero content mounted and a golden test
// can pin the waveform.

// The looping PCM for the compiled-in engine hum and the wind rush. Mono int16 at
// kEngineAudioSampleRate; each buffer is a whole number of cycles so it loops seamlessly (the wind
// noise is filtered CIRCULARLY, so its loop seam is continuous too). Pure, never fails.
enum class EngineLoopKind {
    Engine, // a low buzzy turbine hum: a harmonic stack over an 80 Hz fundamental
    Wind,   // a broadband airframe rush: circularly low-passed deterministic noise (seamless loop)
};

[[nodiscard]] DecodedPcm generateEngineLoopPcm(EngineLoopKind kind);

inline constexpr int kEngineAudioSampleRate = 22050;

// ── the pure throttle/airspeed → pitch/gain mapping (unit-tested without a device) ───────────────

struct EngineToneParams {
    float pitch = 1.f; // OpenAL AL_PITCH multiplier
    float gain = 0.f;  // pre-volume gain [0,1]; the caller multiplies by master*sfx
};

// Engine layer for an aircraft at `throttle01` [0,1], true airspeed `airspeedMps`, afterburner
// `afterburner`. Pitch rises with throttle (spool) and a little with airspeed (ram/whine); the
// afterburner adds a distinct pitch + gain jump. Pure, clamped to a safe OpenAL pitch range.
[[nodiscard]] EngineToneParams engineTone(float throttle01, float airspeedMps, bool afterburner) noexcept;

// Wind/airframe rush gain: scales with DYNAMIC PRESSURE (∝ airspeed²), so it is inaudible taxiing and
// dominant at high speed. Clamped [0, kWindMaxGain]. Pure.
[[nodiscard]] float windRushGain(float airspeedMps) noexcept;

// Wind rush pitch: creeps up with airspeed so the rush "tightens" as you accelerate. Pure.
[[nodiscard]] float windRushPitch(float airspeedMps) noexcept;

// Mapping constants (documented so a content-tuning pass has a reference).
inline constexpr float kEngineIdleGain = 0.22f; // gain at throttle 0
inline constexpr float kEngineMilGain = 0.75f;  // gain at throttle 1 (MIL power)
inline constexpr float kEngineAbGainBoost = 0.20f;
inline constexpr float kEngineBasePitch = 0.75f; // pitch at throttle 0
inline constexpr float kEnginePitchPerThrottle = 0.7f;
inline constexpr float kEngineAbPitchBoost = 0.30f;
inline constexpr float kEngineRamPitchFullMps = 340.f; // airspeed at which the ram term saturates
inline constexpr float kEngineRamPitchBoost = 0.20f;
inline constexpr float kEngineMinPitch = 0.5f;
inline constexpr float kEngineMaxPitch = 2.0f;
inline constexpr float kWindFullMps = 260.f; // airspeed at which the rush reaches full gain
inline constexpr float kWindMaxGain = 0.55f;
inline constexpr float kWindBasePitch = 0.9f;
inline constexpr float kWindPitchPerMps = 0.0008f;

} // namespace fl
