// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/RenderSnapshot.h"

#include <span>

namespace fl {

// Aircraft HUD interface. Displays instrument data (speed, altitude, heading,
// attitude, etc.) in cockpit camera mode.
//
// FlightHud is the builtin implementation. Content packs can provide a custom
// IHud for each aircraft type via the content system (future phase).
class IHud {
  public:
    virtual ~IHud() = default;

    // Build HUD elements for this frame.
    // Pass nullptr when not in Cockpit mode to suppress all output.
    // latencyMs: receiving peer's one-way latency in ms (from SnapshotPeerLatency TLV); 0 = not yet received.
    // showLatency: true when the latency indicator should be rendered (gated by config + threshold +
    // hasSnapshotLatency).
    virtual void update(const EntityRenderEntry* playerEntry, float timeOfDay = 12.0f, float terrainElevation = 0.0f,
                        uint32_t latencyMs = 0, bool showLatency = false) = 0;

    // Returns overlay elements. Valid until the next call to update().
    [[nodiscard]] virtual std::span<const HudElement> elements() const = 0;
};

} // namespace fl
