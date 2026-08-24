// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/SessionComms.h"

#include "INetwork.h"
#include "Utf8Decode.h" // chat text sanitization (#646)
#include "atc/AtcService.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "net/WireCodec.h"
#include "net/WorldBroadcaster.h"
#include "sensor/SensorSystem.h"
#include "sensor/TrackPicture.h" // FusedTrack / TrackFuser — the per-faction datalink fusion (#1088)
#include "world/FactionRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace fl {

// Copy an ATC RadioTransmission into its wire form; snprintf force-terminates each char[] safely.
MsgRadioTransmission buildRadioWire(const fl::atc::RadioTransmission& tx, uint8_t netId) {
    MsgRadioTransmission w{};
    w.netId = netId;
    w.displaySeconds = tx.displaySeconds;
    std::snprintf(w.speaker, sizeof(w.speaker), "%s", tx.speaker.c_str());
    std::snprintf(w.voiceKey, sizeof(w.voiceKey), "%s", tx.voiceKey.c_str());
    std::snprintf(w.text, sizeof(w.text), "%s", tx.text.c_str());
    return w;
}

namespace {
// Sanitize a chat line: keep only printable BMP codepoints (drop control chars, DEL, and any invalid /
// non-BMP sequence which nextUtf8Codepoint reports as U+FFFD), copying the ORIGINAL bytes of each kept
// codepoint so valid multi-byte sequences survive intact, truncate on a codepoint boundary, and trim
// surrounding whitespace.
std::string sanitizeChat(std::string_view in) {
    std::string out;
    const char* p = in.data();
    const char* const end = p + in.size();
    while (p < end && out.size() < fl::kMaxChatBytes) {
        const char* prev = p;
        const uint32_t cp = fl::nextUtf8Codepoint(p, end);
        if (cp < 0x20u || cp == 0x7Fu || cp == 0xFFFDu)
            continue; // control / DEL / invalid
        const std::size_t n = static_cast<std::size_t>(p - prev);
        if (out.size() + n > fl::kMaxChatBytes)
            break;
        out.append(prev, n);
    }
    const auto notSpace = [](char c) { return c != ' ' && c != '\t'; };
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), notSpace));
    out.erase(std::find_if(out.rbegin(), out.rend(), notSpace).base(), out.end());
    return out;
}
} // namespace

SessionComms::SessionComms(WorldBroadcaster& wb, EntityManager& entityManager, INetwork& net,
                           const WorldBroadcasterHooks& hooks) noexcept
    : m_wb(wb), m_entityManager(entityManager), m_net(net), m_hooks(hooks) {
    // The compiled-in radio-net stack (Epic J). Seeded here so voice works with zero configuration;
    // fl-server's [[voice.nets]] replaces it wholesale via setRadioNets().
    for (auto& def : builtinRadioNets())
        m_radioNets.add(def);
}

void SessionComms::queueCombatEvent(const CombatEventRecord& rec) {
    m_pendingKillEvents.push_back(rec);
}

void SessionComms::onDisconnect(uint32_t peerId) {
    m_voiceViewsValid = false; // #1090: a departing peer changes the voice recipient set
    for (auto& [netId, talkers] : m_voiceTalkers)
        talkers.erase(peerId); // free its talker slot immediately rather than after the hold window
}

void SessionComms::resetMatchState() {
    m_pendingKillEvents.clear();
    m_scoreboardDirty = true;
}

void SessionComms::setMotd(std::string motd) {
    m_motd = std::move(motd);
}

void SessionComms::setMotdDisplaySeconds(uint16_t seconds) noexcept {
    m_motdDisplaySeconds = seconds;
}

void SessionComms::sendMotdTo(uint32_t peerId) {
    // Unicast once on admission (#853 moved it off onConnect). Byte-for-byte the block that used to
    // sit inline in the connect handshake.
    if (m_motd.empty())
        return;
    const std::size_t textLen = std::min(m_motd.size(), kMaxMotdBytes);
    MsgMotdHeader mhdr{};
    mhdr.displaySeconds = m_motdDisplaySeconds;
    std::vector<uint8_t> pkt;
    pkt.reserve(sizeof(MsgMotdHeader) + textLen + 1);
    appendMsg(pkt, mhdr);
    pkt.insert(pkt.end(), m_motd.c_str(), m_motd.c_str() + textLen);
    pkt.push_back(0u); // NUL terminator
    m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/true);
}

void SessionComms::flushCombatEvents() {
    // Kill records broadcast to everyone, chunked to stay inside a single unfragmented packet.
    if (!m_pendingKillEvents.empty()) {
        constexpr std::size_t kMaxRecordsPerPacket = 15; // 4 + 15*32 = 484 bytes
        std::vector<uint8_t> buf;
        for (std::size_t off = 0; off < m_pendingKillEvents.size(); off += kMaxRecordsPerPacket) {
            const std::size_t n = std::min(kMaxRecordsPerPacket, m_pendingKillEvents.size() - off);
            buf.clear();
            MsgCombatEventHeader hdr;
            hdr.count = static_cast<uint8_t>(n);
            appendMsg(buf, hdr);
            for (std::size_t i = 0; i < n; ++i)
                appendMsg(buf, m_pendingKillEvents[off + i]);
            m_net.broadcast(buf.data(), buf.size(), /*reliable=*/true);
        }
        m_pendingKillEvents.clear();
    }

    // Per-peer stats, unicast — each peer only ever sees its own tallies.
    for (auto& [peerId, score] : m_wb.m_scores) {
        if (!score.dirty)
            continue;
        score.dirty = false;

        std::vector<uint8_t> buf;
        MsgCombatEventHeader hdr;
        hdr.count = 1;
        appendMsg(buf, hdr);
        CombatEventRecord rec{};
        rec.type = static_cast<uint8_t>(CombatEventType::Stats);
        rec.weaponClass = 0xFF;
        rec.a = score.kills;
        rec.b = score.losses;
        rec.c = score.score;
        appendMsg(buf, rec);
        m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
    }
}

void SessionComms::broadcastDatalink(uint64_t tickIndex) {
    if (m_wb.m_peerEntities.empty())
        return;

    // Group observers by faction ONCE this tick, so fusing a team is a lookup, not a scan-per-peer.
    // A faction-0 (neutral) entity is not on anyone's team and forms no datalink net (below).
    std::unordered_map<uint16_t, std::vector<uint32_t>> factionObservers;
    for (uint32_t idx : m_wb.m_sensorSystem.observerIndices()) {
        const EntityState* st = m_entityManager.getByIndex(idx);
        if (!st || st->dead)
            continue;
        factionObservers[st->factionIndex].push_back(idx);
    }

    // Priority for the track cap: a firing-quality lock, then a positively-identified foe, then state
    // rank, then proximity. A datalink that dropped the bandit locked on your leader to keep a distant
    // friendly would be worse than useless.
    auto stateRankOf = [](sensor::ContactState s) {
        switch (s) {
        case sensor::ContactState::Locked:
            return 3;
        case sensor::ContactState::Detected:
            return 2;
        case sensor::ContactState::Coasting:
            return 1;
        case sensor::ContactState::Lost:
            return 0;
        }
        return 0;
    };
    auto d2 = [](const double a[3], const double b[3]) {
        const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    };

    // Fuse ONCE PER FACTION (#1088, D21), not once per pilot. The fused picture is a property of the
    // TEAM: every same-faction peer was re-merging the identical set of teammate tables and then
    // applying its own-sensor marking on top. At 128 players that was roughly
    // 128 pilots x 64 teammates x <=32 contacts ~= 262,000 merges inside a single tick, six times a
    // second, serial on the sim thread with no JobSystem dispatch and no governor lever to shed it —
    // the highest-risk O(P^2) cost in the codebase. Fusing per faction makes it O(P*C + F*C).
    //
    // Faction 0 is neutral: it fuses with no one, so it gets no cache and falls back to its own table
    // alone below — a lone neutral still sees its own picture but shares nothing.
    std::unordered_map<uint16_t, std::vector<sensor::FusedTrack>> factionPicture;
    factionPicture.reserve(factionObservers.size());
    for (const auto& [faction, observers] : factionObservers) {
        if (faction == 0)
            continue;
        sensor::TrackFuser fuser;
        for (uint32_t obsIdx : observers)
            if (const sensor::ContactTable* t = m_wb.m_sensorSystem.contactsFor(obsIdx))
                fuser.add(*t, /*ownSensor=*/false); // the per-peer bit is overlaid below
        factionPicture.emplace(faction, fuser.tracks());
    }

    std::vector<uint8_t> buf;
    std::unordered_map<uint32_t, uint32_t> ownTargets; // target index -> generation, for the overlay
    std::vector<sensor::FusedTrack> tracks;
    for (const auto& [peerId, eid] : m_wb.m_peerEntities) {
        const EntityState* self = m_entityManager.get(eid);
        if (!self || self->dead)
            continue;
        const uint16_t faction = self->factionIndex;

        // The peer's OWN contacts, as an index->generation lookup for the ownSensor overlay. A peer
        // holding a contact itself must be told so: "my radar has this" and "only the datalink shows
        // me this" drive different HUD treatment. contactsFor is non-null only for an observer, and
        // every live observer is in its faction's group above, so the cache already contains this
        // peer's contributions — only the MARKING is per-peer.
        ownTargets.clear();
        const sensor::ContactTable* own = m_wb.m_sensorSystem.contactsFor(eid.index);
        if (own)
            for (const sensor::Contact& c : *own)
                ownTargets[c.id.index] = c.id.generation;

        tracks.clear();
        if (const auto pit = factionPicture.find(faction); faction != 0 && pit != factionPicture.end()) {
            tracks = pit->second; // the team's fused picture, already sorted by target index
            for (sensor::FusedTrack& t : tracks) {
                const auto oit = ownTargets.find(t.id.index);
                t.ownSensor = oit != ownTargets.end() && oit->second == t.id.generation;
            }
        } else if (own) {
            // Neutral (faction 0), or a faction with no observer group: the peer's own picture only.
            sensor::TrackFuser fuser;
            fuser.add(*own, /*ownSensor=*/true);
            tracks = fuser.tracks();
        }
        if (tracks.size() > kMaxDatalinkTracks) {
            std::sort(tracks.begin(), tracks.end(), [&](const sensor::FusedTrack& a, const sensor::FusedTrack& b) {
                const int pa =
                    (a.firingQuality ? 8 : 0) + (a.ident == sensor::Identification::Foe ? 4 : 0) + stateRankOf(a.state);
                const int pb =
                    (b.firingQuality ? 8 : 0) + (b.ident == sensor::Identification::Foe ? 4 : 0) + stateRankOf(b.state);
                if (pa != pb)
                    return pa > pb;
                const double da = d2(a.lastKnownPos, self->transform.pos);
                const double db = d2(b.lastKnownPos, self->transform.pos);
                if (da != db)
                    return da < db;
                return a.id.index < b.id.index;
            });
            tracks.resize(kMaxDatalinkTracks);
        }

        const sensor::ThreatWarningSet* threats = m_wb.m_sensorSystem.threatsFor(eid.index);
        const std::size_t threatCount = threats ? std::min(threats->size(), kMaxDatalinkThreats) : 0;

        buf.clear();
        MsgDatalinkHeader hdr;
        hdr.trackCount = static_cast<uint16_t>(tracks.size());
        hdr.threatCount = static_cast<uint16_t>(threatCount);
        hdr.tickIndex = tickIndex;
        hdr.origin[0] = self->transform.pos[0];
        hdr.origin[1] = self->transform.pos[1];
        hdr.origin[2] = self->transform.pos[2];
        appendMsg(buf, hdr);

        for (const sensor::FusedTrack& t : tracks) {
            DatalinkTrack r{};
            r.targetIdx = t.id.index;
            r.targetGen = static_cast<uint16_t>(t.id.generation);
            r.typeIndex = t.typeIndex;
            r.factionIndex = t.factionIndex;
            r.state = static_cast<uint8_t>(t.state);
            r.ident = static_cast<uint8_t>(t.ident);
            r.sensorTypeMask = t.sensorTypeMask;
            r.flags = static_cast<uint8_t>((t.firingQuality ? kDatalinkFlagFiringQuality : 0u) |
                                           (t.ownSensor ? kDatalinkFlagOwnSensor : 0u));
            r.relPos[0] = static_cast<float>(t.lastKnownPos[0] - self->transform.pos[0]);
            r.relPos[1] = static_cast<float>(t.lastKnownPos[1] - self->transform.pos[1]);
            r.relPos[2] = static_cast<float>(t.lastKnownPos[2] - self->transform.pos[2]);
            r.relVel[0] = t.lastKnownVel[0];
            r.relVel[1] = t.lastKnownVel[1];
            r.relVel[2] = t.lastKnownVel[2];
            appendMsg(buf, r);
        }

        for (std::size_t i = 0; i < threatCount; ++i) {
            const sensor::ThreatWarning& w = threats->threats[i];
            DatalinkThreat r{};
            r.emitterIdx = w.emitterId.index;
            r.emitterGen = static_cast<uint16_t>(w.emitterId.generation);
            r.emitterTypeIndex = w.emitterTypeIndex;
            r.emitterFactionIndex = w.emitterFactionIndex;
            r.channel = static_cast<uint8_t>(w.channel);
            r.level = static_cast<uint8_t>(w.level);
            // Correlate the emitter with IFF so a friendly emitter reads as benign on the display.
            r.ident = static_cast<uint8_t>(sensor::classifyIff(
                m_wb.m_factionRegistry ? m_wb.m_factionRegistry->relationship(faction, w.emitterFactionIndex)
                                       : sensor::affiliationRelation(faction, w.emitterFactionIndex),
                static_cast<uint8_t>(1u << static_cast<int>(w.channel)), w.level != sensor::ThreatLevel::Search));
            r.relPos[0] = static_cast<float>(w.emitterPos[0] - self->transform.pos[0]);
            r.relPos[1] = static_cast<float>(w.emitterPos[1] - self->transform.pos[1]);
            r.relPos[2] = static_cast<float>(w.emitterPos[2] - self->transform.pos[2]);
            appendMsg(buf, r);
        }

        m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/false);
    }
}

void SessionComms::sendNoticeTo(uint32_t peerId, const char* text) {
    MsgServerNotice notice;
    std::snprintf(notice.text, sizeof(notice.text), "%s", text);
    m_net.send(peerId, &notice, sizeof(notice), /*reliable=*/true);
}

void SessionComms::sendRadioTransmission(const fl::atc::RadioTransmission& tx) {
    const MsgRadioTransmission w = buildRadioWire(tx, m_radioNets.indexOf("atc"));
    if (tx.target.valid()) {
        for (const auto& [pid, eid] : m_wb.m_peerEntities) {
            if (eid == tx.target) {
                m_net.send(pid, &w, sizeof(w), /*reliable=*/true); // unicast to the addressed pilot
                return;
            }
        }
    }
    // No owning peer (an AI flight's clearance) or an undirected line: every peer hears it.
    m_wb.broadcastMsg(w, /*reliable=*/true);
}

void SessionComms::handleRadioCommand(uint32_t peerId, const void* data, std::size_t size) {
    MsgRadioCommand msg;
    if (!readMsg(data, size, msg))
        return; // truncated; silently discard
    msg.command[sizeof(msg.command) - 1] = '\0';

    auto& ps = m_wb.m_peerInputs[peerId];

    // Per-peer rate limit (~m_wb.m_flightCmdRateLimit/s). Silently drop over the limit — a radio flood must
    // not be amplified back at the sender with a reply per rejected packet.
    if (!ps.radioCmd.allow(m_wb.m_clock->now(), static_cast<uint32_t>(m_wb.m_flightCmdRateLimit)))
        return;

    // The flight the command applies to = the requesting peer's own aircraft (invalid for an observer).
    const auto peerEnt = m_wb.m_peerEntities.find(peerId);
    const fl::EntityId flight = (peerEnt != m_wb.m_peerEntities.end()) ? peerEnt->second : fl::EntityId{};

    // Reply directly to the requester (facility-specific clearances come later from the ATC tick).
    auto reply = [&](fl::atc::AtcPhrase phrase, const char* overrideText = nullptr) {
        fl::atc::RadioTransmission tx = fl::atc::makeTransmission(phrase, "Tower", flight);
        if (overrideText)
            tx.text = overrideText;
        const MsgRadioTransmission w = buildRadioWire(tx, m_radioNets.indexOf("atc"));
        m_net.send(peerId, &w, sizeof(w), /*reliable=*/true);
    };

    // Tokenize "atc <subverb> [facility]" or "base <subverb>" (#55).
    std::string_view cmd(msg.command);
    auto nextToken = [](std::string_view& s) -> std::string_view {
        while (!s.empty() && s.front() == ' ')
            s.remove_prefix(1);
        std::size_t sp = s.find(' ');
        std::string_view tok = s.substr(0, sp);
        s.remove_prefix(sp == std::string_view::npos ? s.size() : sp);
        return tok;
    };
    const std::string_view verb = nextToken(cmd);

    // Base operations (#55): ground-crew services. Routed before the ATC gate — the crew chief
    // works whether or not a tower does.
    if (verb == "base") {
        m_wb.handleBaseOpsCommand(peerId, flight, nextToken(cmd));
        return;
    }

    if (!m_wb.m_atcService) {
        reply(fl::atc::AtcPhrase::Unable, "no ATC available");
        return;
    }

    if (verb != "atc") {
        reply(fl::atc::AtcPhrase::Unable, "say again");
        return;
    }
    const std::string_view sub = nextToken(cmd);
    const std::string facility(nextToken(cmd)); // optional; empty = nearest

    if (sub == "request_takeoff") {
        m_wb.m_atcService->requestTakeoff(flight, facility);
        reply(fl::atc::AtcPhrase::Roger);
    } else if (sub == "request_landing") {
        m_wb.m_atcService->requestLanding(flight, facility);
        reply(fl::atc::AtcPhrase::Roger);
    } else if (sub == "inbound") {
        m_wb.m_atcService->declareInbound(flight, facility);
        reply(fl::atc::AtcPhrase::Roger);
    } else if (sub == "cancel") {
        m_wb.m_atcService->cancel(flight);
        reply(fl::atc::AtcPhrase::Roger);
    } else {
        reply(fl::atc::AtcPhrase::Unable, "say again");
    }
}

void SessionComms::sendChatEvent(uint32_t peerId, uint8_t channel, uint32_t senderPeerId, std::string_view text) {
    MsgChatEventHeader hdr;
    hdr.channel = channel;
    hdr.senderPeerId = senderPeerId;
    std::vector<uint8_t> pkt;
    pkt.reserve(sizeof(hdr) + text.size() + 1);
    appendMsg(pkt, hdr);
    pkt.insert(pkt.end(), text.begin(), text.end());
    pkt.push_back('\0');
    m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/true);
}

void SessionComms::handleChat(uint32_t peerId, const void* data, std::size_t size) {
    if (!m_chatEnabled)
        return;
    MsgChatHeader hdr;
    if (!readMsg(data, size, hdr))
        return; // truncated
    if (!isChatChannelOrdinal(hdr.channel))
        return;

    const auto pit = m_wb.m_peerInputs.find(peerId);
    if (pit == m_wb.m_peerInputs.end() || !pit->second.handshakeComplete)
        return; // not admitted
    auto& ps = pit->second;
    if (ps.chatMuted)
        return; // muted: drop silently, no rate-limit warning

    // Extract + sanitize the NUL-terminated text at offset 4.
    const char* bytes = static_cast<const char*>(data);
    std::string_view raw(bytes + sizeof(hdr), size > sizeof(hdr) ? size - sizeof(hdr) : 0u);
    if (const auto z = raw.find('\0'); z != std::string_view::npos)
        raw = raw.substr(0, z);
    const std::string text = sanitizeChat(raw);
    if (text.empty())
        return;

    // Per-peer rate limit (warn once per window; never one reply per rejected packet — a flood must not
    // be amplified back at the sender).
    if (!ps.chat.allow(m_wb.m_clock->now(), static_cast<uint32_t>(m_chatRateLimit))) {
        if (!ps.chat.warned) {
            ps.chat.warned = true;
            sendNoticeTo(peerId, "You are sending chat too fast.");
        }
        return;
    }

    // Moderation hook: false = suppress (fl-server default logs an audit line and allows).
    if (m_hooks.comms.chatModeration && !m_hooks.comms.chatModeration(peerId, hdr.channel, text))
        return;

    // Record what was actually said (#600) -- after the veto, so a suppressed line is absent from
    // the log rather than present-but-unsent, which would make the log disagree with the match.
    {
        MatchEvent me;
        me.type = MatchEventType::Chat;
        me.actor = peerId;
        me.channel = hdr.channel;
        me.factionIndex = m_wb.factionForPeer(peerId);
        me.text = std::string(text);
        m_wb.m_matchEventLog.append(std::move(me));
    }

    // Offer the line to the intent tier (#611) — after the veto and after the record, so a line that
    // was suppressed never reaches a model, and what a model saw is what the match log says was
    // said. Team channel only: the wingman answers to their flight, not to everyone in the server.

    const auto channel = static_cast<ChatChannel>(hdr.channel);
    const uint16_t senderFaction = (channel == ChatChannel::Team) ? m_wb.factionForPeer(peerId) : kNoFaction;
    for (const auto& [pid, pin] : m_wb.m_peerInputs) {
        if (!pin.handshakeComplete)
            continue;
        if (channel == ChatChannel::Team) {
            // Team channel: same faction only. A teamless sender (observer) sees only its own echo.
            if (senderFaction == kNoFaction) {
                if (pid != peerId)
                    continue;
            } else if (m_wb.factionForPeer(pid) != senderFaction) {
                continue;
            }
        }
        sendChatEvent(pid, hdr.channel, peerId, text);
    }
}

bool SessionComms::setPeerMuted(uint32_t peerId, bool muted) {
    const auto it = m_wb.m_peerInputs.find(peerId);
    if (it == m_wb.m_peerInputs.end())
        return false;
    it->second.chatMuted = muted;
    return true;
}

bool SessionComms::isPeerMuted(uint32_t peerId) const {
    const auto it = m_wb.m_peerInputs.find(peerId);
    return it != m_wb.m_peerInputs.end() && it->second.chatMuted;
}

// ---------------------------------------------------------------------------------------------
// Voice comms (#532)
// ---------------------------------------------------------------------------------------------
// The server's entire involvement with audio is: check the sender may talk on this net, work out
// who is on it, and copy the bytes. It never decodes a frame — which is what makes voice for 128
// players cost the server almost nothing, and what lets the codec change without a protocol change.

void SessionComms::setRadioNets(RadioNetTable nets) {
    m_radioNets = std::move(nets);
    if (m_radioNets.empty()) {
        // A server that configures no nets still gets a working radio rather than silent voice.
        for (auto& def : builtinRadioNets())
            m_radioNets.add(def);
    }
}

bool SessionComms::setPeerVoiceMuted(uint32_t peerId, bool muted) {
    const auto it = m_wb.m_peerInputs.find(peerId);
    if (it == m_wb.m_peerInputs.end())
        return false;
    it->second.voiceMuted = muted;
    m_voiceViewsValid = false; // #1090: mute state is part of the recipient set
    return true;
}

std::vector<uint32_t> SessionComms::voiceMutedPeers() const {
    std::vector<uint32_t> out;
    for (const auto& [pid, ps] : m_wb.m_peerInputs) {
        if (ps.voiceMuted)
            out.push_back(pid);
    }
    std::sort(out.begin(), out.end());
    return out;
}

VoicePeerView SessionComms::voicePeerView(uint32_t peerId) const {
    VoicePeerView v;
    v.peerId = peerId;
    const auto pit = m_wb.m_peerInputs.find(peerId);
    if (pit == m_wb.m_peerInputs.end())
        return v;
    v.admitted = pit->second.handshakeComplete;
    v.voiceMuted = pit->second.voiceMuted;

    const auto eit = m_wb.m_peerEntities.find(peerId);
    if (eit == m_wb.m_peerEntities.end())
        return v; // observer / not yet spawned: admitted, but with no team, flight or position
    if (const EntityState* st = m_entityManager.get(eit->second)) {
        // Faction 0 is NEUTRAL, not "teamless" — matching handleChat, where neutral players form
        // their own team. Only the kNoFaction sentinel (no entity at all) means teamless.
        v.faction = st->factionIndex;
        v.hasPosition = true;
        v.pos[0] = st->transform.pos[0];
        v.pos[1] = st->transform.pos[1];
        v.pos[2] = st->transform.pos[2];
    }
    // "My flight" is the formation holding this aircraft as a member, or — for a flight lead, who is
    // the ANCHOR rather than a member — the formation anchored on it. Missing either reading would
    // silently exclude every lead from their own flight net.
    fl::FormationId fid = m_wb.m_formations.formationOfEntity(eit->second);
    if (fid == fl::kNoFormation)
        fid = m_wb.m_formations.formationAnchoredOn(eit->second);
    v.formationId = fid;
    return v;
}

void SessionComms::buildVoicePeerViews(std::vector<VoicePeerView>& out) const {
    out.clear();
    out.reserve(m_wb.m_peerInputs.size());
    for (const auto& [pid, ps] : m_wb.m_peerInputs) {
        if (!ps.handshakeComplete)
            continue;
        out.push_back(voicePeerView(pid));
    }
}

void SessionComms::sendVoiceNetDefs(uint32_t peerId) {
    std::vector<uint8_t> buf;
    MsgVoiceNetDefHeader hdr;
    const bool on = m_voiceEnabled && !m_radioNets.empty();
    hdr.flags = on ? kVoiceServerEnabled : 0u;
    hdr.netCount = on ? static_cast<uint8_t>(m_radioNets.size()) : 0u;
    buf.reserve(sizeof(hdr) + (on ? m_radioNets.size() * sizeof(MsgVoiceNetRecord) : 0u));
    appendMsg(buf, hdr);
    if (on) {
        for (std::size_t i = 0; i < m_radioNets.size(); ++i) {
            const RadioNetDef& def = m_radioNets.nets()[i];
            MsgVoiceNetRecord rec{};
            rec.netId = static_cast<uint8_t>(i);
            rec.kind = static_cast<uint8_t>(def.kind);
            rec.flags = static_cast<uint8_t>((def.positional ? kVoiceNetFlagPositional : 0u) |
                                             (def.radioEffect ? kVoiceNetFlagRadioEffect : 0u) |
                                             (def.defaultNet ? kVoiceNetFlagDefault : 0u));
            rec.rangeM = def.rangeM;
            rec.gain = def.gain;
            std::snprintf(rec.id, sizeof(rec.id), "%s", def.id.c_str());
            std::snprintf(rec.name, sizeof(rec.name), "%s", def.name.c_str());
            appendMsg(buf, rec);
        }
    }
    // Reliable: a client that never learns the table cannot key a mic at all, and the table is one
    // packet per connect.
    m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
}

void SessionComms::handleVoiceFrame(uint32_t peerId, const void* data, std::size_t size) {
    if (!m_voiceEnabled)
        return;
    MsgVoiceFrameHeader hdr;
    if (!readMsg(data, size, hdr))
        return; // truncated

    const auto pit = m_wb.m_peerInputs.find(peerId);
    if (pit == m_wb.m_peerInputs.end() || !pit->second.handshakeComplete)
        return; // not admitted
    auto& ps = pit->second;

    // Length validation is the ONLY thing we can do to a payload nobody on this machine will ever
    // decode; do it before anything else touches the bytes.
    const std::size_t avail = size > sizeof(hdr) ? size - sizeof(hdr) : 0u;
    const std::size_t payloadBytes = std::min<std::size_t>(hdr.payloadBytes, avail);
    if (hdr.payloadBytes > kMaxVoiceFrameBytes || payloadBytes != hdr.payloadBytes)
        return;

    // Bandwidth bound (see PeerInputState). Dropped silently: a reply to a flood is amplification.
    if (!ps.voice.allow(m_wb.m_clock->now(), static_cast<uint32_t>(m_voiceFrameRateLimit)))
        return;

    // Concurrent-speaker cap (#1090, D20). The relay cost of a net is (talkers x listeners) and only
    // the listener side was ever bounded: 128 open mics at 128 players is ~975,000 sendChannel calls
    // a second, roughly 78 MB/s, against ~32k/s for a realistic five-talker session. First come
    // keeps its slot; a talker holds the slot through a brief gap so a pause for breath does not drop
    // it mid-sentence. Over the cap the frame is dropped silently — answering would amplify.
    {
        const RadioNetDef* net = m_radioNets.byIndex(hdr.netId);
        if (net && net->maxTalkers > 0) {
            constexpr auto kVoiceTalkerHoldMs = std::chrono::milliseconds(250);
            const auto now = m_wb.m_clock->now();
            auto& talkers = m_voiceTalkers[hdr.netId];
            for (auto it = talkers.begin(); it != talkers.end();)
                it = (now - it->second >= kVoiceTalkerHoldMs) ? talkers.erase(it) : std::next(it);
            const auto mine = talkers.find(peerId);
            if (mine != talkers.end()) {
                mine->second = now; // already holding a slot: refresh it
            } else {
                if (talkers.size() >= static_cast<std::size_t>(net->maxTalkers))
                    return; // the net is at capacity this moment
                talkers.emplace(peerId, now);
            }
        }
    }

    const VoicePeerView sender = voicePeerView(peerId);
    // Rebuild the peer views at most ONCE PER TICK (#1090) rather than on every received frame. The
    // list is an O(P) walk and the picture it describes changes per tick, not per 20 ms frame.
    if (!m_voiceViewsValid || m_voiceViewsTick != m_wb.m_currentTick) {
        buildVoicePeerViews(m_voicePeerScratch);
        m_voiceViewsTick = m_wb.m_currentTick;
        m_voiceViewsValid = true;
    }
    if (!selectVoiceRecipients(m_radioNets, hdr.netId, sender, m_voicePeerScratch, m_voiceRecipientScratch))
        return; // unknown net, muted, or no membership on this net
    if (m_voiceRecipientScratch.empty())
        return; // nobody on the net — a common and completely normal case

    MsgVoiceRelayHeader rel;
    rel.netId = hdr.netId;
    rel.seq = hdr.seq;
    rel.flags = hdr.flags;
    rel.senderPeerId = peerId;
    rel.payloadBytes = static_cast<uint16_t>(payloadBytes);
    rel.senderEntityIdx = kNoVoiceEntity;
    if (const auto eit = m_wb.m_peerEntities.find(peerId); eit != m_wb.m_peerEntities.end())
        rel.senderEntityIdx = eit->second.index;

    m_voiceRelayScratch.clear();
    m_voiceRelayScratch.reserve(sizeof(rel) + payloadBytes);
    appendMsg(m_voiceRelayScratch, rel);
    const auto* bytes = static_cast<const uint8_t*>(data) + sizeof(hdr);
    m_voiceRelayScratch.insert(m_voiceRelayScratch.end(), bytes, bytes + payloadBytes);

    // Unreliable, on the dedicated voice channel: a lost frame is 20 ms the receiver conceals, and
    // retransmitting it would deliver it after the moment it belonged to. The dedicated channel
    // keeps ENet's per-channel unreliable sequencing from making voice and snapshots drop each
    // other (see kNetChVoice).
    for (const uint32_t rid : m_voiceRecipientScratch)
        m_net.sendChannel(rid, m_voiceRelayScratch.data(), m_voiceRelayScratch.size(), /*reliable=*/false, kNetChVoice);
    m_voiceRelaySends += m_voiceRecipientScratch.size(); // #1090: fan-out is what voice actually costs
}

std::vector<uint32_t> SessionComms::mutedPeers() const {
    std::vector<uint32_t> out;
    for (const auto& [pid, ps] : m_wb.m_peerInputs)
        if (ps.chatMuted)
            out.push_back(pid);
    std::sort(out.begin(), out.end());
    return out;
}

void SessionComms::appendScoreboardRows(std::vector<uint8_t>& pkt, std::size_t begin, std::size_t count,
                                        const std::vector<uint32_t>& order) const {
    for (std::size_t i = begin; i < begin + count; ++i) {
        const uint32_t pid = order[i];
        const auto sit = m_wb.m_scores.find(pid);
        ScoreboardRow row{};
        row.participantId = pid;
        if (sit != m_wb.m_scores.end()) {
            row.score = sit->second.score;
            row.kills = static_cast<uint16_t>(std::min<uint32_t>(sit->second.kills, 0xFFFFu));
            row.deaths = static_cast<uint16_t>(std::min<uint32_t>(sit->second.losses, 0xFFFFu));
        }
        // Ping: humans carry their estimatedDelayTicks; bots have none.
        if (!isBotParticipant(pid)) {
            const auto pit = m_wb.m_peerInputs.find(pid);
            if (pit != m_wb.m_peerInputs.end()) {
                const uint32_t ms = static_cast<uint32_t>(m_wb.m_tickRate.ticksToMs(pit->second.estimatedDelayTicks));
                row.pingMs = static_cast<uint16_t>(std::min<uint32_t>(ms, 0xFFFFu));
            }
        }
        // Team from the roster (the authoritative team record) else the live entity.
        const auto rit = m_wb.m_roster.find(pid);
        row.factionIndex = (rit != m_wb.m_roster.end()) ? rit->second.factionIndex : m_wb.factionForPeer(pid);
        appendMsg(pkt, row);
    }
}

void SessionComms::buildScoreboardPackets(std::vector<std::vector<uint8_t>>& out) {
    out.clear();
    if (m_wb.m_scores.empty())
        return;
    std::vector<uint32_t> order;
    order.reserve(m_wb.m_scores.size());
    for (const auto& [pid, sc] : m_wb.m_scores) {
        (void)sc;
        order.push_back(pid);
    }
    std::sort(order.begin(), order.end()); // deterministic
    for (std::size_t i = 0; i < order.size(); i += kMaxScoreboardRowsPerPacket) {
        const std::size_t n = std::min(kMaxScoreboardRowsPerPacket, order.size() - i);
        MsgScoreboardHeader hdr{};
        hdr.count = static_cast<uint8_t>(n);
        std::vector<uint8_t> pkt;
        pkt.reserve(sizeof(hdr) + n * sizeof(ScoreboardRow));
        appendMsg(pkt, hdr);
        appendScoreboardRows(pkt, i, n, order);
        out.push_back(std::move(pkt));
    }
    ++m_scoreboardBuilds; // instrumentation: one build per dirty window, not one per peer
}

void SessionComms::sendScoreboardTo(uint32_t peerId) {
    // Unicast path (a late joiner on admit): one peer, so building for it is the whole job.
    buildScoreboardPackets(m_scoreboardScratch);
    for (const std::vector<uint8_t>& pkt : m_scoreboardScratch)
        m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/false);
}

void SessionComms::broadcastScoreboard() {
    // Build ONCE, then send (#1091). The rows are receiver-independent — every peer was handed
    // byte-identical content built 128 separate times, re-sorting every participant each pass: about
    // 90,000 appends and 640 sends every two seconds for work with exactly one correct result.
    // flushCombatEvents already had this shape for the kill feed; the scoreboard simply never got it.
    buildScoreboardPackets(m_scoreboardScratch);
    if (m_scoreboardScratch.empty())
        return;
    for (const std::vector<uint8_t>& pkt : m_scoreboardScratch)
        m_wb.broadcastBytes(pkt, /*reliable=*/false, /*admittedOnly=*/true);
}

} // namespace fl
