// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Thin factory header. Include this instead of SDL3AudioCapture.h so consumers are never exposed
// to SDL3 headers (the OALAudioFactory / VkRendererFactory precedent).
#include "IAudioCapture.h"

#include <memory>

namespace fl {

// Always returns a valid object; whether a device opens is decided by init(). Never null, so the
// caller has one failure path (init returning false), not two.
std::unique_ptr<IAudioCapture> createSDL3AudioCapture();

} // namespace fl
