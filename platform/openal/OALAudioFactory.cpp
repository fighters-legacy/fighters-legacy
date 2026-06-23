// SPDX-License-Identifier: GPL-3.0-or-later
#include "OALAudioFactory.h"
#include "OALAudio.h"

namespace fl {

std::unique_ptr<IAudio> createOALAudio() {
    return std::make_unique<OALAudio>();
}

} // namespace fl
