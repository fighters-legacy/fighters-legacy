// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Callback interface implemented by the game layer and invoked from the sim thread each tick.
// IMPORTANT: onTick() runs on the sim thread. Do NOT call any HAL methods here.
// The only HAL method safe to call from the sim thread is ILogger::log().
class ISimUpdate {
  public:
    virtual ~ISimUpdate() = default;

    // Called once per fixed sim tick.
    // simDt:     fixed timestep in seconds (= 1/tickRate, default 1/60 s).
    // tickIndex: monotonically increasing tick counter.
    virtual void onTick(double simDt, uint64_t tickIndex) = 0;
};

} // namespace fl
