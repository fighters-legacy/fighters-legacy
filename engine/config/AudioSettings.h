// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// All fields are float 0.0–1.0 for direct use with IAudio::setGain().
// config/user.toml stores these as integers 0–100 (matching the "0–100%" spec);
// the conversion is handled in UserConfig::load() and save().
struct AudioSettings {
    float masterVolume = 0.80f;    // TOML: 80
    float sfxVolume = 1.00f;       // TOML: 100
    float musicVolume = 0.70f;     // TOML: 70
    float voiceChatVolume = 1.00f; // TOML: 100
    float rwrVolume = 1.00f;       // TOML: 100
};
