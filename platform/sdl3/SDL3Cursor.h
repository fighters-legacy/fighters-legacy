// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ICursor.h"
#include <string>

struct SDL_Cursor; // forward declaration — SDL3/SDL.h only in .cpp

namespace fl {

class SDL3Cursor : public ICursor {
  public:
    ~SDL3Cursor() override;

    void setCursor(CursorShape shape) override;
    void setCustomCursor(const void* pixels, int width, int height, int hotX, int hotY) override;
    const char* getLastError() const override;

  private:
    void releaseCurrentCursor();

    SDL_Cursor* m_cursor{nullptr}; // cursor we own and must destroy; nullptr when using default
    bool m_hidden{false};
    mutable std::string m_lastError;
};

} // namespace fl
