// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WireRateSampler (#772) — turns a backend's CUMULATIVE wire counters into the rate form of
// `fl::WireStats`. Pure logic (no sockets, no clock of its own: the caller passes the timestamp),
// so it unit-tests directly.
//
// Why this exists: the two transports report wire traffic in different shapes. ENet keeps
// cumulative host totals (`ENetHost::totalSentData` etc.); GNS reports per-connection *rates* and
// no lifetime totals at all. WireStats is a rate because that is the honest intersection — GNS
// fills it natively, ENet goes through this sampler.
//
// ENet's counters are `enet_uint32` and DO wrap ("user should reset to 0 as needed to prevent
// overflow" — enet.h). At 128 clients (~9 MB/s) `totalSentData` wraps roughly every 8 minutes, so a
// soak run wraps repeatedly. Unsigned 32-bit subtraction is exact across a single wrap, so the
// delta is correct as long as the sampling interval is shorter than the wrap period — true by a
// wide margin (we sample sub-second). We therefore never reset the counters, which would race with
// anything else reading them.

#include "INetwork.h"
#include <cstdint>

namespace fl {

class WireRateSampler {
  public:
    // Feeds the current cumulative counters and the current time (seconds, monotonic). Returns the
    // rates since the previous call. The FIRST call primes the sampler and returns all-zero: there
    // is no interval to divide by yet, and inventing one would report a spike.
    WireStats sample(uint32_t outBytes, uint32_t inBytes, uint32_t outPackets, uint32_t inPackets, double nowS) {
        if (!m_primed) {
            m_primed = true;
            m_lastOutBytes = outBytes;
            m_lastInBytes = inBytes;
            m_lastOutPackets = outPackets;
            m_lastInPackets = inPackets;
            m_lastT = nowS;
            return {};
        }
        const double dt = nowS - m_lastT;
        if (dt <= 0.0)
            return m_last; // clock didn't advance: repeat the last rate rather than divide by ~0

        // Unsigned wrap-around subtraction: exact across a single uint32 wrap.
        const uint32_t dOutBytes = outBytes - m_lastOutBytes;
        const uint32_t dInBytes = inBytes - m_lastInBytes;
        const uint32_t dOutPackets = outPackets - m_lastOutPackets;
        const uint32_t dInPackets = inPackets - m_lastInPackets;

        WireStats s;
        s.outBytesPerSec = static_cast<double>(dOutBytes) / dt;
        s.inBytesPerSec = static_cast<double>(dInBytes) / dt;
        s.outPacketsPerSec = static_cast<double>(dOutPackets) / dt;
        s.inPacketsPerSec = static_cast<double>(dInPackets) / dt;

        m_lastOutBytes = outBytes;
        m_lastInBytes = inBytes;
        m_lastOutPackets = outPackets;
        m_lastInPackets = inPackets;
        m_lastT = nowS;
        m_last = s;
        return s;
    }

  private:
    bool m_primed{false};
    uint32_t m_lastOutBytes{0};
    uint32_t m_lastInBytes{0};
    uint32_t m_lastOutPackets{0};
    uint32_t m_lastInPackets{0};
    double m_lastT{0.0};
    WireStats m_last{};
};

} // namespace fl
