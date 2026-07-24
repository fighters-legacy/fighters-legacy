// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/GameProtocol.h"
#include "voice/RadioNet.h"
#include "voice/VoiceCodec.h" // constants only; engine-net does not link the codec

#include <cstdint>
#include <vector>

namespace fl {

// The wire cap and the codec cap are two independent constants in two layers that must not link to
// each other (engine-protocol is zero-dependency by policy). This is the one place that sees both.
static_assert(kMaxVoiceFrameBytes == kMaxVoicePayloadBytes,
              "GameProtocol kMaxVoiceFrameBytes and VoiceCodec kMaxVoicePayloadBytes must agree");

inline constexpr uint16_t kNoVoiceFaction = 0xFFFFu;
inline constexpr uint32_t kNoVoiceFormation = 0u; // matches fl::kNoFormation

// Everything the router needs to know about one peer. A flat POD view rather than a reference into
// WorldBroadcaster's maps, so the routing rules are testable without a server, a world, or a
// transport — which is the whole point of pulling them out.
struct VoicePeerView {
    uint32_t peerId{0};
    uint16_t faction{kNoVoiceFaction};
    uint32_t formationId{kNoVoiceFormation};
    bool admitted{false};    // handshake complete; an unadmitted peer neither speaks nor hears
    bool voiceMuted{false};  // admin-muted: may not TRANSMIT (still hears everyone)
    bool hasPosition{false}; // false for an observer / a dead peer: no aircraft to be near
    double pos[3]{0.0, 0.0, 0.0};
};

// ---------------------------------------------------------------------------------------------
// Voice routing (#532)
// ---------------------------------------------------------------------------------------------
// Pure functions over a net table and a peer list. The server calls these per received frame and
// then relays the payload untouched — it never decodes, mixes, or transcodes audio.
//
// The sender is ALWAYS excluded from the recipient set. Echoing a speaker their own voice back
// across the network is a latency-delayed copy of what they just said, which is the single most
// disorienting thing a voice system can do; local sidetone, if we ever want it, belongs on the
// client where it costs no round trip.
// ---------------------------------------------------------------------------------------------

// May `sender` transmit on `net` at all? A muted peer may not; a Team net needs a faction and a
// Flight net needs a formation, because otherwise "my team" and "my flight" have no referent and
// the frame would either go nowhere or, worse, go everywhere.
[[nodiscard]] bool canTransmitOn(const RadioNetDef& net, const VoicePeerView& sender) noexcept;

// Would `listener` receive a transmission `sender` makes on `net`? Sender-exclusive.
[[nodiscard]] bool voiceReaches(const RadioNetDef& net, const VoicePeerView& sender,
                                const VoicePeerView& listener) noexcept;

// Fill `out` with the peer ids that should receive this transmission. `out` is cleared first.
// Returns false (with `out` empty) when the sender may not transmit on this net at all.
bool selectVoiceRecipients(const RadioNetTable& nets, uint8_t netId, const VoicePeerView& sender,
                           const std::vector<VoicePeerView>& peers, std::vector<uint32_t>& out);

} // namespace fl
