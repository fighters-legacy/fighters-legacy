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
    // ⚠ Null on the MISSION path, and structurally so — not an oversight (#1236). Mission objects
    // spawn from applyMission inside ServerRuntime::initMission, while the ATC service is
    // constructed in initSystems because its scramble handler needs the GameLoop, which initAdmin
    // builds after initMission has already run. So at mission-spawn time no service exists to pass.
    // Consequence, stated plainly: a Lua script attached by a mission (`ai: lua ...` or a type's
    // default script) gets atc.* as safe no-ops, while the same script attached by the admin
    // `spawn` command reaches the live service. Closing that gap means moving ATC/GameLoop
    // construction ahead of initMission — a phase reordering with its own teardown-order rules,
    // deliberately not smuggled into a consolidation change.
    atc::AtcService* atcService{nullptr};
};

struct AiControllerBuild {
    std::unique_ptr<IEntityController> controller;
    AiBuildError error{AiBuildError::None};
    std::string detail; // LuaController::lastError() when error == LuaScriptError
};

[[nodiscard]] AiControllerBuild buildAiController(const AiControllerRequest& req);

} // namespace fl
