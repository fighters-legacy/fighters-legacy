// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/IWorldAiProvider.h"

// The no-provider provider (#163).
//
// This is what runs when `[ai_provider] enabled = false`, when no plugin is configured, or when one
// fails to load — which is to say, it is the path CI tests and the path most servers run
// (docs/ai-architecture.md §7: "CI never requires a model"). It is not a placeholder.
//
// It supports NOTHING and says so through supports(), so a caller degrades to its scripted path by
// asking rather than by discovering. Every request is REFUSED at the call — returning 0 — rather
// than accepted and completed with an empty result: "I cannot do this" reaches the caller
// immediately and synchronously, where "here is nothing, eventually" would look like a working
// provider with a bad model behind it.

namespace fl {

class NullAiProvider final : public IWorldAiProvider {
  public:
    bool init(ILogger& logger) override;
    void shutdown() override;
    void service() override;

    [[nodiscard]] bool supports(WorldAiCapability cap) const override;
    [[nodiscard]] const char* getLastError() const override;

    WorldAiRequestId requestMission(const WorldAiContext& ctx,
                                    std::function<void(std::string, std::string)> cb) override;
    WorldAiRequestId requestWorldEvolution(const WorldAiContext& ctx,
                                           std::function<void(WorldEvolutionDelta, std::string)> cb) override;
    WorldAiRequestId requestNarrative(NarrativeType type, const WorldAiContext& ctx,
                                      std::function<void(std::string, std::string)> cb) override;
    WorldAiRequestId requestFactionDecision(const std::string& factionId, const WorldAiContext& ctx,
                                            std::function<void(FactionDecision, std::string)> cb) override;
    WorldAiRequestId requestIntent(const WorldAiContext& ctx,
                                   std::function<void(std::string, std::string)> cb) override;

    void cancel(WorldAiRequestId id) override;

  private:
    ILogger* m_logger{nullptr};
};

} // namespace fl
