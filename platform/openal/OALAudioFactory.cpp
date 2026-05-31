// SPDX-License-Identifier: GPL-3.0-or-later
#include "OALAudioFactory.h"
#include "OALAudio.h"

std::unique_ptr<IAudio> createOALAudio() {
    return std::make_unique<OALAudio>();
}
