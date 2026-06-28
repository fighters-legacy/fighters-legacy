// SPDX-License-Identifier: GPL-3.0-or-later
#include "BotClient.h"
#include "ENetNetworkFactory.h"
#include <net/GameProtocol.h>
#include <net/WireCodec.h>

namespace fl {

BotClient::BotClient(uint32_t index, const std::string& patternName, int rateHz)
    : m_index(index), m_rateHz(rateHz), m_pattern(makePattern(patternName, index + 1u)) {}

BotClient::~BotClient() = default;

bool BotClient::connect(double now, const char* host, uint16_t port) {
    m_now = now;
    m_connectStart = now;
    m_net = createENetNetwork();
    if (!m_net || !m_net->init())
        return false;
    m_net->setEventHandler(this);
    return m_net->connect(host, port);
}

void BotClient::service() {
    if (m_net)
        m_net->service(0);
}

void BotClient::sendInputIfDue(double now) {
    m_now = now;
    if (!m_metrics.connected || !m_net)
        return;
    const double interval = 1.0 / static_cast<double>(m_rateHz);
    if (m_lastInputAt >= 0.0 && (now - m_lastInputAt) < interval)
        return;
    m_lastInputAt = now;

    const BotControl ctl = m_pattern->sample(now - m_activeStart, m_index);
    MsgClientInput in;
    in.buttons = ctl.buttons;
    in.seqNum = m_seq++;
    in.tickIndex = m_lastTick;
    in.throttle = ctl.throttle;
    in.elevator = ctl.elevator;
    in.aileron = ctl.aileron;
    in.rudder = ctl.rudder;
    m_net->send(0, &in, sizeof(in), /*reliable=*/false);
    ++m_metrics.inputsSent;
}

void BotClient::sampleRtt() {
    if (m_net && m_metrics.connected) {
        m_metrics.rttMs = m_net->getPeerRtt(0);
        m_metrics.rttValid = true;
    }
}

void BotClient::beginDisconnect() {
    if (m_net)
        m_net->disconnect();
}

void BotClient::shutdown() {
    if (m_net) {
        m_net->shutdown();
        m_net.reset();
    }
}

void BotClient::onConnect(uint32_t /*peerId*/) {
    m_metrics.connected = true;
    m_metrics.connectMs = (m_now - m_connectStart) * 1000.0;
    m_activeStart = m_now;
}

void BotClient::onDisconnect(uint32_t /*peerId*/) {
    // A disconnect after a successful connect, while the version was fine, is an unexpected
    // drop during the run (flood-kick, idle-timeout, crash) — flag it. A version-mismatch
    // disconnect we triggered ourselves is expected and not counted.
    if (m_metrics.connected && m_versionOk)
        m_metrics.disconnectedDuringRun = true;
    m_metrics.connected = false;
}

void BotClient::onReceive(uint32_t /*peerId*/, const void* data, std::size_t size) {
    if (size < 1)
        return;
    const uint8_t msgId = static_cast<const uint8_t*>(data)[0];
    switch (static_cast<MsgId>(msgId)) {
    case MsgId::Hello: {
        MsgHello h;
        if (readMsg(data, size, h) && h.protocolVersion != kProtocolVersion) {
            m_versionOk = false;
            if (m_net)
                m_net->disconnect();
        }
        break;
    }
    case MsgId::WorldSnapshot: {
        MsgWorldSnapshotHeader hdr;
        if (readMsg(data, size, hdr)) {
            m_lastTick = hdr.tickIndex;
            m_metrics.snapshotBytes += size;
            ++m_metrics.snapshotCount;
            if (m_metrics.snapshotCount == 1) {
                m_metrics.firstSnapshotTick = hdr.tickIndex;
                m_metrics.firstSnapshotWall = m_now;
            } else {
                const double gapMs = (m_now - m_metrics.lastSnapshotWall) * 1000.0;
                if (gapMs > m_metrics.maxSnapshotGapMs)
                    m_metrics.maxSnapshotGapMs = gapMs;
            }
            m_metrics.lastSnapshotTick = hdr.tickIndex;
            m_metrics.lastSnapshotWall = m_now;
        }
        break;
    }
    case MsgId::PeerDelay: {
        MsgPeerDelay pd;
        if (readMsg(data, size, pd) && pd.delayTicks > 0) {
            m_metrics.rttMs = static_cast<uint32_t>(pd.delayTicks) * 1000u / 60u;
            m_metrics.rttValid = true;
        }
        break;
    }
    default:
        break; // ConnectAck / weather / motd / etc. — not needed for load metrics
    }
}

} // namespace fl
