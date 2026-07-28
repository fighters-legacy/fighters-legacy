// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/MatchEventLog.h"
#include "net/WorldState.h"
#include "world/AlertLevel.h"
#include "world/FactionDef.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The generative-AI provider seam (#163).
//
// Connects fl-server to any external model — Anthropic, OpenAI-compatible, Ollama, or something an
// operator wrote — for GENERATIVE CONTENT: missions, campaign events, narrative text, faction
// decisions, and the free-text intent mapping #611 rides on. Distinct from the Lua AI (#33), which
// scripts unit behaviour. This issue ships the interface and NullAiProvider; concrete backends are
// follow-ons.
//
// ── WHY THIS IS ITS OWN TARGET (engine-worldai), not part of engine-world ─────────────────────────
//
// Plan #1036 D5 said WorldAiContext is built ON WorldStateSnapshot rather than being a parallel
// struct, and it was right: one source of truth for "what agents see", shared with the MCP
// world_state tool. But WorldStateSnapshot and MatchEvent live in engine-net, and engine-net
// PUBLIC-links engine-world — so putting this in engine-world (as #163's body assumed) would be a
// dependency cycle. It sits ABOVE both instead. Same reasoning that gave engine-replay its own
// target in Stage 2: a seam that consumes two layers belongs above them, not inside one.
//
// ── THREADING ────────────────────────────────────────────────────────────────────────────────────
//
// A model call takes 100 ms to 10 s and must NEVER block the sim thread. The contract is
// IAsyncFilesystem's, exactly, because that is the async shape this codebase already has:
//
//   request*()  dispatches on the provider's own thread(s) and returns immediately
//   service()   drains a completion queue and fires callbacks ON THE CALLING THREAD
//   shutdown()  cancels in-flight work and joins; safe if init() was never called
//
// Callbacks therefore run wherever fl-server calls service() — its main loop, not the sim thread —
// and every result is passed BY VALUE, so a provider cannot hand out a pointer into a buffer it is
// about to reuse.

namespace fl {

class ILogger;

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
//
// A provider declares what it can actually do. A backend wired to a small local model may map intent
// well and write narrative badly, and the caller needs to know that BEFORE it degrades a feature to
// its scripted path — silently returning empty from an unsupported request would look like a model
// that is merely slow.
enum class WorldAiCapability : uint8_t {
    Mission = 0,         // generate a mission YAML document
    WorldEvolution = 1,  // decide how the world moves between sorties
    Narrative = 2,       // briefing / debrief / radio chatter / after-action text
    FactionDecision = 3, // diplomatic and posture decisions for one faction
    Intent = 4,          // free text -> one scripted wingman command (#611)
    Count = 5,
};

[[nodiscard]] inline constexpr bool isWorldAiCapabilityOrdinal(uint8_t v) noexcept {
    return v < static_cast<uint8_t>(WorldAiCapability::Count);
}

[[nodiscard]] const char* worldAiCapabilityName(WorldAiCapability c) noexcept;

enum class NarrativeType : uint8_t {
    Briefing = 0,
    Debrief = 1,
    RadioChatter = 2,
    AfterAction = 3,
};

// ---------------------------------------------------------------------------
// What an agent sees
// ---------------------------------------------------------------------------

// A zone's posture, flattened for a prompt (#162's AlertSystem is the source).
struct ZoneSummary {
    std::string id;
    uint16_t ownerFactionIndex{0};
    AlertLevel alertLevel{AlertLevel::Peacetime};
    EscalationStage stage{EscalationStage::Clean};
};

// The context every request carries. Built on the published WorldStateSnapshot (D5) rather than a
// bespoke struct, so the MCP `world_state` tool and the provider see the SAME world.
//
// `snapshot` may be null when the server has not published one yet; a provider must handle that
// rather than assume a server has been up long enough to have a world.
struct WorldAiContext {
    std::shared_ptr<const WorldStateSnapshot> snapshot;

    std::string theaterName;
    std::string campaignName;
    int dayIndex{0};

    std::vector<ZoneSummary> zones;
    std::vector<MatchEvent> recentEvents;   // MatchEventLog tail — what just happened
    std::vector<std::string> entityTypeIds; // the spawn vocabulary; anything else is invalid
    std::string operatorHint;               // free text from server.toml / an admin command

    // Free text from a player, for Intent requests only. UNTRUSTED, ALWAYS — it is typed by someone
    // who may be trying to talk past the prompt. A provider must template it as DATA, and the caller
    // validates whatever comes back against the scripted grammar regardless (#611).
    std::string utterance;
};

// ---------------------------------------------------------------------------
// What an agent may change
// ---------------------------------------------------------------------------

struct ZoneControlChange {
    std::string zoneId;
    uint16_t newOwnerFactionIndex{0};
};
struct AlertLevelChange {
    uint16_t factionIndex{0};
    AlertLevel level{AlertLevel::Peacetime};
};
struct RelationshipChange {
    uint16_t factionA{0};
    uint16_t factionB{0};
    FactionRelation relation{FactionRelation::Neutral};
};
struct SpawnEvent {
    std::string entityTypeId; // must be in WorldAiContext::entityTypeIds; anything else is dropped
    uint16_t factionIndex{0};
    double worldPos[3]{0.0, 0.0, 0.0};
    double headingDeg{0.0};
};

// The delta a world-evolution request returns. Every field is optional; an empty delta is a valid
// answer meaning "nothing changed today", which is different from an error.
struct WorldEvolutionDelta {
    std::vector<ZoneControlChange> zoneChanges;
    std::vector<AlertLevelChange> alertChanges;
    std::vector<RelationshipChange> relationshipChanges;
    std::vector<SpawnEvent> spawnEvents;
    std::string narrativeText;
};

struct FactionDecision {
    std::string factionId;
    AlertLevel postureChange{AlertLevel::Peacetime};
    bool changePosture{false};
    std::vector<RelationshipChange> relationshipChanges;
    std::string rationale; // shown to operators, never executed
};

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

// Opaque handle for an in-flight request. 0 = the request could not be enqueued (before init(),
// after shutdown(), or the capability is unsupported) — the IAsyncFilesystem::readFileAsync
// convention, so a caller checks one thing in one way across both seams.
using WorldAiRequestId = uint32_t;

class IWorldAiProvider {
  public:
    virtual ~IWorldAiProvider() = default;

    // Start whatever the backend needs (threads, a client, a warm connection). False on failure;
    // getLastError() says why. The object stays valid and inert after a failed init.
    virtual bool init(ILogger& logger) = 0;

    // Cancel in-flight requests and join. Every pending callback fires with a non-empty error, so a
    // caller waiting on one is never left waiting forever. Safe if init() was never called.
    virtual void shutdown() = 0;

    // Drain completions and fire callbacks on THIS thread. Called once per fl-server main-loop pass.
    virtual void service() = 0;

    // What this backend can actually do. A caller degrades to its scripted path on false.
    [[nodiscard]] virtual bool supports(WorldAiCapability cap) const = 0;

    // Human-readable description of the last failure, or nullptr. Valid until the next call.
    [[nodiscard]] virtual const char* getLastError() const = 0;

    // Every request takes the context by const& and copies what it needs; every callback receives
    // its result BY VALUE plus an error string that is empty on success. A callback fires exactly
    // once per accepted request.

    // Mission YAML. NEVER loaded directly: the caller runs it through validateMission first (#601's
    // submit_mission is the same gate reached a different way).
    virtual WorldAiRequestId requestMission(const WorldAiContext& ctx,
                                            std::function<void(std::string yaml, std::string error)> cb) = 0;

    virtual WorldAiRequestId
    requestWorldEvolution(const WorldAiContext& ctx,
                          std::function<void(WorldEvolutionDelta delta, std::string error)> cb) = 0;

    virtual WorldAiRequestId requestNarrative(NarrativeType type, const WorldAiContext& ctx,
                                              std::function<void(std::string text, std::string error)> cb) = 0;

    virtual WorldAiRequestId
    requestFactionDecision(const std::string& factionId, const WorldAiContext& ctx,
                           std::function<void(FactionDecision decision, std::string error)> cb) = 0;

    // Map ctx.utterance onto ONE scripted wingman command name (#611). The provider returns a
    // grammar NAME, not an action: the caller parses it with fl::ai::parseWingmanCommand and
    // executes through the same path the radio menu drives, so a provider that invents a command —
    // or is talked into one — cannot actuate anything.
    //
    // "unknown" is the correct answer for an utterance outside the grammar, and declining is
    // preferred over guessing.
    virtual WorldAiRequestId requestIntent(const WorldAiContext& ctx,
                                           std::function<void(std::string commandName, std::string error)> cb) = 0;

    // Best-effort cancellation. The callback still fires — with an error — so no caller is left
    // waiting on a request that will never complete.
    virtual void cancel(WorldAiRequestId id) = 0;

    // Factory symbol a plugin exports, following IContentPack::kFactorySymbol exactly:
    //   extern "C" fl::IWorldAiProvider* fighters_legacy_create_ai_provider();
    static constexpr const char* kFactorySymbol = "fighters_legacy_create_ai_provider";
};

using WorldAiProviderFactory = IWorldAiProvider* (*)();

} // namespace fl
