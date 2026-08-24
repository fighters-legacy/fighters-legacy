// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The ONE AI-controller construction ladder (#1236).
//
// Three copies of "Lua script if we have one, else the C++ factory, else nothing" lived in this
// binary — the admin `spawn` command and twice inside the mission `onSpawned` lambda — and they had
// already drifted: the admin path handed LuaController the ATC service, both mission-path
// constructions did not, so the SAME script got the atc.* module (#705) or silently did not
// depending on which block built it.
//
// What this helper does NOT do: resolve the script source. The two callers reach different caches
// (the admin path's `loadAIScript` env hook vs the mission path's preloaded `aiScriptCache`), so the
// caller resolves the text and the helper builds from it. It also does not format errors — it
// classifies them, because the two callers report through different channels (the admin shell echo
// vs the server log) with different, operator-facing wording that must not change.

#include <atc/AtcService.h>
#include <entity/IEntityController.h>
#include <script/LuaSandbox.h> // ScriptPackSource

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

class EntityManager;
struct WorldApi;

enum class AiBuildError {
    None,
    LuaScriptError,  // the Lua source loaded but LuaController rejected it; `detail` is lastError()
    UnknownBehavior, // the factory did not recognise the behavior name or its args
};

struct AiControllerRequest {
    // A non-empty luaSource selects the Lua path; otherwise a non-empty behavior (other than the
    // bare "lua" selector) selects the factory path. Neither = no controller and no error.
    std::string luaSource;
    ScriptPackSource luaPack;
    std::string behavior;
    std::vector<std::string> args;

    const EntityManager* entityManager{nullptr};
    const WorldApi* worldApi{nullptr};
    // Every caller that HAS a service passes it, and all of them do when [atc] enabled = true
    // (#1288). It used to be null on the mission path — mission objects spawn from applyMission
    // inside initMission, and the service was built in initSystems because the block that built it
    // also wired the scramble handler onto the GameLoop, which does not exist until initAdmin. So
    // the same Lua script reached the live atc.* module (#705) or got safe no-ops depending only on
    // which code path constructed its controller.
    //
    // The fix was to split construction from wiring rather than reorder the phases: AtcService needs
    // only (entityManager, airportRegistry, planetR), all ready by initWorld, so it is built there
    // and only setSpawnHandler still waits for the GameLoop in initSystems. Between the two the API
    // fails closed — AtcService::scramble returns false with no handler — and nothing can call it
    // anyway, because scripts do not run until mainLoop starts the sim.
    //
    // Still null when [atc] enabled = false, on both paths, and atc.* is safe no-ops.
    atc::AtcService* atcService{nullptr};
};

struct AiControllerBuild {
    std::unique_ptr<IEntityController> controller;
    AiBuildError error{AiBuildError::None};
    std::string detail; // LuaController::lastError() when error == LuaScriptError
};

[[nodiscard]] AiControllerBuild buildAiController(const AiControllerRequest& req);

} // namespace fl
