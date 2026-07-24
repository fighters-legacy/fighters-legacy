// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ArtChannel — the articulation channel vocabulary (#840, Epic #837).
//
// THE CONTRACT, verified against DCS (numbered draw arguments over baked EDM keyframes), X-Plane
// (named datarefs driving OBJ8 ANIM_rotate/ANIM_trans), Microsoft Flight Simulator (baked glTF
// node-TRS clips scrubbed by a simvar → scale/bias → clamp 0..1 mapping) and Falcon BMS (numbered
// DOF nodes with multiplier and limits) — they all implement the same one:
//
//   The ENGINE owns named, normalized channels. The MODEL bakes keyframed node-TRS clips. The
//   runtime SCRUBS the clip at t = value x duration — it never "plays" it.
//
// So retraction is scrubbing `gear` toward 0, not a second `gear_retract` clip: a second clip is
// duplicate state to keep in sync and matches no shipping sim. Rigid mechanical parts are plain
// animated NODES, never skins and never morph targets.
//
// SPIN IS THE ONE EXCEPTION to scrubbing: prop/rotor/wheel clips loop at a channel-driven rate.
//
// This header is deliberately dependency-free (stdlib only) so the wire codec, the flight model, the
// validators and the renderer can all name a channel without pulling in a rig, a glTF parser or a
// GPU type. THE ENUM ORDER IS THE WIRE ORDER AND IS ABI — APPEND ONLY.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fl {

enum class ArtChannel : uint8_t {
    Gear = 0,          // 0 = up, doors closed; 1 = down-locked. One clip sequences struts, doors, linkages.
    Flaps,             // 0 = clean, 1 = full. Slats may live in the same clip.
    Speedbrake,        // 0 = stowed, 1 = deployed
    Hook,              // 0 = stowed, 1 = down
    Canopy,            // 0 = closed, 1 = open
    Sweep,             // 0 = WingSweepData::min_deg, 1 = max_deg
    TvcPitch,          // signed: -1 = TvcData::min_angle_deg, +1 = max_angle_deg
    TvcYaw,            // signed; reserved (the sim is pitch-only today)
    Elevator,          // signed: -1 = full nose-down, +1 = full nose-up
    Aileron,           // signed: +1 = right-roll command; one clip animates both surfaces
    Rudder,            // signed: +1 = right yaw
    PropRate,          // LOOPING: playhead advances at rate x (1/duration) rev/s
    Bay,               // 0 = closed, 1 = open — the weapons-bay slot (#583)
    GearCompressNose,  // 0 = extended, 1 = bottomed; client-derived, never sent
    GearCompressLeft,  //
    GearCompressRight, //
    kCount
};

inline constexpr std::size_t kArtChannelCount = static_cast<std::size_t>(ArtChannel::kCount);

// Clip name == channel name, lowercase_underscore. `prop_spin` also answers to `rotor_spin` and
// `wheel_spin` — one channel, three names, because a helicopter's rotor and a rolling wheel are the
// same looping playhead as a propeller and splitting them would be three ways to say one thing.
[[nodiscard]] constexpr std::string_view artChannelName(ArtChannel c) noexcept {
    switch (c) {
    case ArtChannel::Gear:
        return "gear";
    case ArtChannel::Flaps:
        return "flaps";
    case ArtChannel::Speedbrake:
        return "speedbrake";
    case ArtChannel::Hook:
        return "hook";
    case ArtChannel::Canopy:
        return "canopy";
    case ArtChannel::Sweep:
        return "sweep";
    case ArtChannel::TvcPitch:
        return "tvc_pitch";
    case ArtChannel::TvcYaw:
        return "tvc_yaw";
    case ArtChannel::Elevator:
        return "elevator";
    case ArtChannel::Aileron:
        return "aileron";
    case ArtChannel::Rudder:
        return "rudder";
    case ArtChannel::PropRate:
        return "prop_spin";
    case ArtChannel::Bay:
        return "bay";
    case ArtChannel::GearCompressNose:
        return "gear_compress_nose";
    case ArtChannel::GearCompressLeft:
        return "gear_compress_left";
    case ArtChannel::GearCompressRight:
        return "gear_compress_right";
    case ArtChannel::kCount:
        break;
    }
    return {};
}

// Resolve a clip name to its channel. Returns kCount for an unknown name — an authoring typo, which
// validate-mesh warns about by name rather than letting it fail silently at runtime.
[[nodiscard]] constexpr ArtChannel artChannelFromName(std::string_view name) noexcept {
    // The spin aliases: one looping playhead, three authoring names.
    if (name == "rotor_spin" || name == "wheel_spin")
        return ArtChannel::PropRate;
    for (std::size_t i = 0; i < kArtChannelCount; ++i) {
        const auto c = static_cast<ArtChannel>(i);
        if (artChannelName(c) == name)
            return c;
    }
    return ArtChannel::kCount;
}

// Signed channels map v ∈ [-1, +1] onto the clip: t = (v + 1) / 2 x duration. The clip's start is
// full negative deflection, its midpoint neutral, its end full positive. ASYMMETRIC CONTROL AUTHORITY
// (max_elevator_deg != max_elevator_neg_deg) is expressed by authoring asymmetric endpoints, NOT by
// rescaling the parameter — rescaling would make the neutral position depend on the flight model.
[[nodiscard]] constexpr bool artChannelIsSigned(ArtChannel c) noexcept {
    switch (c) {
    case ArtChannel::TvcPitch:
    case ArtChannel::TvcYaw:
    case ArtChannel::Elevator:
    case ArtChannel::Aileron:
    case ArtChannel::Rudder:
        return true;
    default:
        return false;
    }
}

// The looping channel: its value is a RATE (revolutions per second at clip duration 1), not a
// position, so the runtime advances a phase accumulator instead of scrubbing to an absolute time.
[[nodiscard]] constexpr bool artChannelIsSpin(ArtChannel c) noexcept {
    return c == ArtChannel::PropRate;
}

// Gate an attacker-supplied ordinal before casting it (the wire carries a channel mask, #843).
[[nodiscard]] constexpr bool isArtChannelOrdinal(uint8_t v) noexcept {
    return v < static_cast<uint8_t>(ArtChannel::kCount);
}

// Neutral value of a channel — the pose an entity that never commands it holds. Signed channels rest
// at 0 (the clip midpoint), unsigned at 0 (stowed/up/clean). Both are 0; the function exists so the
// intent is stated once rather than assumed at every default-initialization site.
[[nodiscard]] constexpr float artChannelNeutral(ArtChannel) noexcept {
    return 0.0f;
}

} // namespace fl
