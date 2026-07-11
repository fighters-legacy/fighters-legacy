// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LossyProxy — a local UDP relay that degrades the loopback link for the bot_swarm congestion
// gate (#714). Synthetic clients connect to the proxy's listen port instead of fl-server; the
// proxy forwards datagrams both ways through a LossyLink policy (drop fraction + added one-way
// delay inside a scheduled window), giving the server's per-peer congestion controller (#518)
// real ENet-visible loss/RTT to react to — then a clean tail to recover in.
//
// Portable by construction: one background I/O thread over poll()/WSAPoll (the RconServer
// idiom); each distinct client (source addr:port) gets its own connected upstream socket so
// fl-server sees N distinct peers. IPv4 loopback only — this is a harness instrument, not a
// general relay. All platform headers/#ifdefs are confined to LossyProxy.cpp (pimpl, like
// platform/Subprocess).

#include "LossyLink.h"
#include <cstdint>
#include <memory>

namespace fl {

class LossyProxy {
  public:
    LossyProxy();
    ~LossyProxy();

    LossyProxy(const LossyProxy&) = delete;
    LossyProxy& operator=(const LossyProxy&) = delete;

    // Binds 127.0.0.1:<ephemeral> and starts the relay thread forwarding to serverHost:serverPort.
    // Returns false on any socket failure (callers should abort the run — a half-proxy would
    // silently un-degrade the link). listenPort() is valid after a successful start().
    bool start(const char* serverHost, uint16_t serverPort, const LossySchedule& sched, uint32_t seed = 1u);

    // Stops the relay thread and closes all sockets. Idempotent; also run by the destructor.
    void stop();

    uint16_t listenPort() const;

    // Relay counters (approximate, for the end-of-run log line): datagrams forwarded and dropped.
    uint64_t forwardedCount() const;
    uint64_t droppedCount() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
