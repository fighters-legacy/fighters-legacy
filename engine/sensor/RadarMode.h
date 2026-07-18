// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl::sensor {

// How an observer is operating its RADAR (#526). This is runtime state, not a sensor def: the same
// APG-66 def is flown Silent one moment and locked in STT the next. It governs only radar-typed
// sensors — an IRST or the eyeball is unaffected, because they are passive and there is nothing to
// switch off.
//
//   * Silent  — the radar is not radiating (EMCON). No radar contacts at all (a radar sees nothing
//               it does not first illuminate), and the emitter is invisible to hostile RWR. The
//               passive suite (IRST, eyeball) keeps working.
//   * Search  — the radar sweeps a volume and reports bearing + range (Detected). It does NOT hold a
//               firing-quality track: a target found in Search must be locked before a radar missile
//               has a solution. Hostile RWR reads a search strobe.
//   * Tws     — track-while-scan: the radar scans AND maintains soft tracks on everything in the
//               cone. Contacts can reach the Locked state, but the lock is NOT firing-quality
//               (`Contact::firingQuality` stays false) — TWS spreads its energy and cannot provide
//               the continuous illumination a SARH shot needs. Hostile RWR reads a scan, not a lock.
//   * Stt     — single-target-track: the radar dedicates itself to ONE designated target, holding a
//               continuous firing-quality lock (`firingQuality == true`) that enables SARH guidance
//               and paints the target hard — hostile RWR reads a steady lock tone. Every other target
//               is invisible to the radar while it is in STT (it is not scanning).
enum class RadarMode : uint8_t { Silent = 0, Search = 1, Tws = 2, Stt = 3 };

// Gate an attacker-supplied byte (MsgClientInput::radarMode) before casting it to RadarMode.
[[nodiscard]] inline bool isRadarModeOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(RadarMode::Stt);
}

// True when this mode has the radar radiating (Search/TWS/STT). Silent is the only non-emitting mode.
[[nodiscard]] inline bool radarModeEmits(RadarMode mode) noexcept {
    return mode != RadarMode::Silent;
}

} // namespace fl::sensor
