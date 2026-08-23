// SPDX-License-Identifier: GPL-3.0-or-later
#include "HeadTracker.h"

namespace fl {

HeadTracker::~HeadTracker() {
    stop();
}

bool HeadTracker::start(uint16_t port) {
    stop();
    m_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!sockValid(m_sock)) {
        stop();
        return false;
    }
    setNonBlocking(m_sock);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // opentrack streams to localhost
    if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        stop();
        return false; // port already in use etc. — non-fatal, head tracking just stays off
    }
    m_running = true;
    return true;
}

void HeadTracker::stop() {
    if (sockValid(m_sock)) {
        closeSocket(m_sock);
        m_sock = kInvalidSocket;
    }
    m_running = false;
}

void HeadTracker::poll(float dt, const HeadTrackingSettings& cfg) {
    RawHeadPose latest;
    bool haveLatest = false;
    if (m_running) {
        // Drain to the newest datagram — an old queued frame is stale for a 60 Hz view.
        for (;;) {
            unsigned char buf[64];
            const auto n = recvfrom(m_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0)
                break;
            if (auto p = parseOpentrackDatagram(buf, static_cast<std::size_t>(n))) {
                latest = *p;
                haveLatest = true;
            }
        }
    }
    m_filter.update(haveLatest ? &latest : nullptr, dt, cfg);
}

} // namespace fl
