// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `.flrep` — the on-disk replay format (#643), implementing docs/developer/replay-format.md (#187).
//
// The spec is the contract; this header is the code side of it and deliberately adds nothing the
// document does not describe. Read the document for WHY the fields exist -- particularly why
// planetRadiusM is stored rather than assumed (every geodetic readout on playback is a function of
// it, and an assumed Earth would render a plausible WRONG altitude instead of failing loudly).
//
// The one rule to keep in mind while editing this file: additive changes bump the MINOR version and
// a reader skips what it does not know via each section's declared length; anything that changes the
// meaning of an existing field bumps the MAJOR, and a reader refuses a newer major outright. The
// version is CHECKED because .flrep crosses a build boundary — a player downloads a stranger's file
// written by a build that no longer exists (decision record D18, docs/developer/architecture.md).

#include "net/MatchEventLog.h" // MatchEvent -- the ONE event record (#600 D1), not a second vocabulary

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

inline constexpr char kReplayMagic[4] = {'F', 'L', 'R', 'P'};

// Bump MINOR for a new optional section or fields appended to a section's tail. Bump MAJOR when a
// field's meaning changes, a section is removed, or the record layout changes -- and expect every
// older reader to refuse the file, which is the point.
inline constexpr uint16_t kReplayFormatMajor = 1;
inline constexpr uint16_t kReplayFormatMinor = 0;

// Section ids. 0x0004 is RESERVED (camera track) rather than free: #41 does not need it, and an id
// claimed later by something else is how two builds end up disagreeing about the same number.
enum class ReplaySectionId : uint16_t {
    EntityTypeManifest = 0x0001,
    FactionTable = 0x0002,
    Roster = 0x0003,
    CameraTrack = 0x0004, // reserved; not written, skipped on read
};

// Per-tick flags.
inline constexpr uint16_t kReplayTickKeyframe = 0x0001u;

// Session flags in the header.
inline constexpr uint32_t kReplaySessionMission = 0x0001u; // the session ran a mission
inline constexpr uint32_t kReplaySessionScored = 0x0002u;  // a match was scored

// Hard caps applied to anything a FILE declares. A `.flrep` is something a player downloads from a
// stranger, so every declared count is bounded before it is believed, let alone allocated on.
inline constexpr uint32_t kReplayMaxSectionBytes = 64u * 1024u * 1024u;
inline constexpr uint32_t kReplayMaxChunkBytes = 64u * 1024u * 1024u;
inline constexpr uint32_t kReplayMaxStringBytes = 4096u;
inline constexpr uint32_t kReplayMaxEntityTypes = 65535u;
inline constexpr uint32_t kReplayMaxFactions = 65535u;
inline constexpr uint32_t kReplayMaxRosterEntries = 65535u;
inline constexpr uint32_t kReplayMaxTicksPerChunk = 65535u;

// A `.flrep` keyframe is a tick whose entity records are ALL full. SnapshotCodec has no separate
// keyframe concept -- full-vs-delta is caller policy expressed by a per-record presence bit -- so
// the format needs no new codec, only the discipline of emitting every entity full on a cadence.
// Keyframes are the seek points, so the cadence trades seek granularity against file size.
inline constexpr uint32_t kReplayDefaultKeyframeIntervalTicks = 120; // 2 s at 60 Hz; see #643's measurement

// --- Header ---------------------------------------------------------------

struct ReplayHeader {
    uint16_t formatMajor{kReplayFormatMajor};
    uint16_t formatMinor{kReplayFormatMinor};
    std::string engineVersion; // the recorder's version, for diagnostics and the refusal message
    uint32_t tickRateHz{60};   // playback timing derives from this, never a hardcoded constant
    double planetRadiusM{0.0}; // load-bearing: every geodetic conversion is a function of it
    uint64_t startUnixSeconds{0};
    std::string missionId; // empty for a free-flight session
    uint32_t sessionFlags{0};
    uint32_t keyframeIntervalTicks{kReplayDefaultKeyframeIntervalTicks};
};

// --- Sections -------------------------------------------------------------

// The replay's equivalent of the MsgEntityTypeDef set a live client receives in ConnectAck: without
// it a stored typeIndex is a number with no meaning.
struct ReplayEntityType {
    uint32_t typeIndex{0};
    std::string id;
    std::string name;
    uint8_t category{0};       // ObjectCategory ordinal
    uint8_t projectileKind{0}; // ProjectileKind ordinal
};

// factionIndex -> id/name (the MsgFactionDef mapping).
struct ReplayFaction {
    uint16_t factionIndex{0};
    std::string id;
    std::string name;
};

// participant id -> callsign: what lets a replay say "Maverick" rather than "entity 7", and what an
// ACMI export (#923) needs for per-object metadata. Participants who join AFTER recording starts are
// not in this section -- they arrive as Join events in the tick stream, which carry their callsign
// for exactly this reason, so a reader builds the roster forward as it reads.
struct ReplayRosterEntry {
    uint32_t participantId{0};
    std::string callsign;
    uint16_t factionIndex{0};
    uint8_t role{0}; // PeerRole ordinal
    bool isBot{false};
};

struct ReplaySections {
    std::vector<ReplayEntityType> entityTypes;
    std::vector<ReplayFaction> factions;
    std::vector<ReplayRosterEntry> roster;
};

// --- Tick -----------------------------------------------------------------

// One recorded tick: the quantized entity record stream exactly as the wire encodes it, plus the
// match events that happened on that tick. Interleaving them keeps a kill and the frame it happened
// on inseparable -- what a killcam, a debrief and an ACMI event line all need.
struct ReplayTick {
    uint64_t tickIndex{0};
    uint16_t flags{0};            // kReplayTickKeyframe
    uint16_t recordCount{0};      // entity records in `records`
    std::vector<double> origins;  // originCount * 3 shared quantization origins
    std::vector<uint8_t> records; // stitched SnapshotCodec record stream
    std::vector<MatchEvent> events;

    [[nodiscard]] bool keyframe() const noexcept {
        return (flags & kReplayTickKeyframe) != 0u;
    }
};

} // namespace fl
