// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

union SDL_Event;

namespace fl {

// Internal interface used by SDL3Window to forward SDL events to SDL3Input.
// Not part of platform-hal — never include this outside platform/sdl3/.
class ISDL3EventSink {
  public:
    virtual ~ISDL3EventSink() = default;
    virtual void onSDLEvent(const SDL_Event& ev) = 0;
};

} // namespace fl
