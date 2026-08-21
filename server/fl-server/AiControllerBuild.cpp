// SPDX-License-Identifier: GPL-3.0-or-later
#include "AiControllerBuild.h"

#include <ai/AiControllerFactory.h>
#include <script/LuaController.h>

#include <span>

namespace fl {

AiControllerBuild buildAiController(const AiControllerRequest& req) {
    AiControllerBuild out;

    if (!req.luaSource.empty()) {
        // Lua AI controller — constructed on the sim thread; the world.* seam (#413) lets the
        // script reach spawn/faction/mission/music, and atcService (when the caller has one) the
        // atc.* module (#705).
        auto lua = std::make_unique<LuaController>(req.luaSource, req.luaPack, req.entityManager, req.worldApi,
                                                   req.atcService);
        if (lua->isValid()) {
            out.controller = std::move(lua);
        } else {
            out.error = AiBuildError::LuaScriptError;
            out.detail = lua->lastError();
        }
        return out;
    }

    if (!req.behavior.empty() && req.behavior != "lua") {
        // C++ AI controller via the factory.
        std::vector<std::string_view> argViews;
        argViews.reserve(req.args.size());
        for (const auto& a : req.args)
            argViews.push_back(a);
        out.controller = ai::createController(req.behavior, std::span<std::string_view>(argViews), req.entityManager);
        if (!out.controller)
            out.error = AiBuildError::UnknownBehavior;
    }

    return out;
}

} // namespace fl
