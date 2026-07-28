// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/NullAiProvider.h"

#include <ILogger.h>

namespace fl {

namespace {
constexpr const char* kNoProvider = "no AI provider configured";
} // namespace

bool NullAiProvider::init(ILogger& logger) {
    m_logger = &logger;
    return true;
}

void NullAiProvider::shutdown() {
    m_logger = nullptr;
}

void NullAiProvider::service() {
    // Nothing is ever in flight, so there is nothing to drain. Deliberately not an error to call:
    // fl-server services the provider unconditionally every pass, and making the no-provider case
    // special would put a branch in the main loop to save nothing.
}

bool NullAiProvider::supports(WorldAiCapability) const {
    return false;
}

const char* NullAiProvider::getLastError() const {
    return kNoProvider;
}

// Every request refuses AT THE CALL rather than completing empty later. The callback is not invoked
// at all — a request that was never accepted has no completion, and a caller that checks the
// returned id (or supports()) never registers one.
WorldAiRequestId NullAiProvider::requestMission(const WorldAiContext&, std::function<void(std::string, std::string)>) {
    return 0;
}

WorldAiRequestId NullAiProvider::requestWorldEvolution(const WorldAiContext&,
                                                       std::function<void(WorldEvolutionDelta, std::string)>) {
    return 0;
}

WorldAiRequestId NullAiProvider::requestNarrative(NarrativeType, const WorldAiContext&,
                                                  std::function<void(std::string, std::string)>) {
    return 0;
}

WorldAiRequestId NullAiProvider::requestFactionDecision(const std::string&, const WorldAiContext&,
                                                        std::function<void(FactionDecision, std::string)>) {
    return 0;
}

WorldAiRequestId NullAiProvider::requestIntent(const WorldAiContext&, std::function<void(std::string, std::string)>) {
    return 0;
}

void NullAiProvider::cancel(WorldAiRequestId) {
    // No request was ever accepted, so there is none to cancel.
}

} // namespace fl
