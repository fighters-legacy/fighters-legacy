// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/VoiceRouter.h"

namespace fl {
namespace {

double distanceSq(const VoicePeerView& a, const VoicePeerView& b) noexcept {
    const double dx = a.pos[0] - b.pos[0];
    const double dy = a.pos[1] - b.pos[1];
    const double dz = a.pos[2] - b.pos[2];
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

bool canTransmitOn(const RadioNetDef& net, const VoicePeerView& sender) noexcept {
    if (!sender.admitted || sender.voiceMuted)
        return false;
    switch (net.kind) {
    case RadioNetKind::Global:
    case RadioNetKind::Atc:
        return true;
    case RadioNetKind::Team:
        // No faction means "my team" has no referent. Refusing is the safe reading: the alternative
        // is a teamless observer's transmission silently reaching either nobody or everybody.
        return sender.faction != kNoVoiceFaction;
    case RadioNetKind::Flight:
        return sender.formationId != kNoVoiceFormation;
    case RadioNetKind::Proximity:
        // Proximity is defined by where your aircraft IS. An observer has no position and so no
        // proximity; they can still listen (see voiceReaches), they just have nothing to be near.
        return sender.hasPosition;
    }
    return false;
}

bool voiceReaches(const RadioNetDef& net, const VoicePeerView& sender, const VoicePeerView& listener) noexcept {
    if (!listener.admitted)
        return false;
    if (listener.peerId == sender.peerId)
        return false; // never echo a speaker to themselves across the network
    switch (net.kind) {
    case RadioNetKind::Global:
    case RadioNetKind::Atc:
        return true;
    case RadioNetKind::Team:
        return listener.faction != kNoVoiceFaction && listener.faction == sender.faction;
    case RadioNetKind::Flight:
        return listener.formationId != kNoVoiceFormation && listener.formationId == sender.formationId;
    case RadioNetKind::Proximity: {
        if (!listener.hasPosition)
            return false;
        if (net.rangeM <= 0.f)
            return true; // an unbounded proximity net is just a global net; honour it literally
        const double r = static_cast<double>(net.rangeM);
        return distanceSq(sender, listener) <= r * r;
    }
    }
    return false;
}

bool selectVoiceRecipients(const RadioNetTable& nets, uint8_t netId, const VoicePeerView& sender,
                           const std::vector<VoicePeerView>& peers, std::vector<uint32_t>& out) {
    out.clear();
    const RadioNetDef* net = nets.byIndex(netId);
    if (!net)
        return false;
    if (!canTransmitOn(*net, sender))
        return false;
    for (const auto& p : peers) {
        if (voiceReaches(*net, sender, p))
            out.push_back(p.peerId);
    }
    return true;
}

} // namespace fl
