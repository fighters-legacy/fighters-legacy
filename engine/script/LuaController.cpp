// SPDX-License-Identifier: GPL-3.0-or-later
#include "script/LuaController.h"
#include "script/LuaSandbox.h"
#include "script/WorldApi.h"

// Lua is compiled as C++ (see cmake/dependencies.cmake), so its symbols have C++
// linkage — no extern "C" wrapper, and never lua.hpp (#1015).
#include <lauxlib.h>
#include <lua.h>

#include "ai/Guidance.h"
#include "atc/AtcService.h" // atc.* Lua module (#705)
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "sensor/SensorSystem.h"
#include "spatial/SpatialIndex.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fl {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct LuaController::Impl {
    std::unique_ptr<LuaSandbox> sandbox;
    const fl::EntityManager* entityManager{nullptr};
    // The whole context, not just the spatial index: detected_contacts() (#691) reads ctx.contacts
    // through this same pointer. Set for the duration of one compute_control pcall, cleared on every
    // exit path — a Lua script must never see a stale view of a previous tick's world.
    const fl::AiTickContext* currentCtx{nullptr};
    uint64_t currentTick{0}; // for contact age_s (ticks since last seen × dt)
    double currentDt{0.0};
    // The entity this controller is flying, for the atc.* self-service bindings (#705). Set at the top
    // of each sample() before the Lua pcall — valid only while a script is executing.
    fl::EntityId currentSelfId{};
    // atc.* module (#705). Thread-safe (AtcService locks internally); null = every atc.* call is nil/false.
    fl::atc::AtcService* atcService{nullptr};
    bool valid{false};
    std::string lastError;
    uint64_t nextErrorLogTick{0}; // rate-limit: log at most once per 60 ticks

    // Coroutine control-flow mode (#412). When the script defines `ai_main` we drive it as a Lua
    // coroutine resumed once per tick, instead of calling `compute_control`. The two are mutually
    // exclusive per LuaController; ai_main wins if both are present. `coroutine` is a thread created
    // from the sandbox state and kept alive via a registry ref (else it is GC'd between ticks); it is
    // owned by the sandbox's lua_State and closed when the sandbox is destroyed.
    bool useCoroutine{false};
    lua_State* coroutine{nullptr};
    int coroutineRef{-2}; // LUA_NOREF; set once the thread is created
    bool coroutineStarted{false};
    bool coroutineDead{false}; // ai_main returned (or errored): all further ticks are neutral

    // world.* module (#413). worldApi is the host seam for engine integration (spawn/faction/music/
    // mission); null = those calls are safe no-ops. elapsedS accumulates sim-dt each tick for
    // world.get_elapsed_time and the world.timer countdown. on_trigger/timer are pure Lua: predicate
    // and callback functions are anchored in the registry and evaluated each tick.
    const fl::WorldApi* worldApi{nullptr};
    double elapsedS{0.0};
    struct TriggerReg {
        int predRef{-2};
        int cbRef{-2};
    };
    struct TimerReg {
        double fireAt{0.0};
        int cbRef{-2};
    };
    std::vector<TriggerReg> triggers;
    std::vector<TimerReg> timers;
};

// ---------------------------------------------------------------------------
// Stack helpers
//
// These may hold C++ objects freely. Lua is compiled as C++ (see
// cmake/dependencies.cmake), so an error raised anywhere below unwinds to the
// enclosing lua_pcall as a C++ exception and destroys locals on the way out.
// Before #1015 this file maintained a hand-checked "no live C++ object when Lua
// raises" rule across all 22 registered functions; nothing enforced it, and the
// equivalent rule in LuaSandbox.cpp was being violated silently.
// ---------------------------------------------------------------------------

static void readVec3(lua_State* L, int idx, double out[3]) {
    lua_getfield(L, idx, "x");
    out[0] = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "y");
    out[1] = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "z");
    out[2] = lua_tonumber(L, -1);
    lua_pop(L, 1);
}

static void readQuat(lua_State* L, int idx, float out[4]) {
    lua_getfield(L, idx, "x");
    out[0] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, idx, "y");
    out[1] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, idx, "z");
    out[2] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, idx, "w");
    out[3] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
}

static void pushVec3d(lua_State* L, const double v[3]) {
    lua_newtable(L);
    lua_pushnumber(L, v[0]);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v[1]);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v[2]);
    lua_setfield(L, -2, "z");
}

static void pushVec3f(lua_State* L, const float v[3]) {
    lua_newtable(L);
    lua_pushnumber(L, static_cast<double>(v[0]));
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, static_cast<double>(v[1]));
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, static_cast<double>(v[2]));
    lua_setfield(L, -2, "z");
}

static void pushEntityState(lua_State* L, const fl::EntityState& s) {
    lua_newtable(L);
    int t = lua_gettop(L);

    pushVec3d(L, s.transform.pos);
    lua_setfield(L, t, "pos");

    pushVec3f(L, s.transform.vel);
    lua_setfield(L, t, "vel");

    // quat = {x, y, z, w}
    lua_newtable(L);
    lua_pushnumber(L, static_cast<double>(s.transform.quat[0]));
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, static_cast<double>(s.transform.quat[1]));
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, static_cast<double>(s.transform.quat[2]));
    lua_setfield(L, -2, "z");
    lua_pushnumber(L, static_cast<double>(s.transform.quat[3]));
    lua_setfield(L, -2, "w");
    lua_setfield(L, t, "quat");

    lua_pushnumber(L, static_cast<double>(s.hp));
    lua_setfield(L, t, "hp");
    lua_pushnumber(L, static_cast<double>(s.maxHp));
    lua_setfield(L, t, "max_hp");
    lua_pushinteger(L, static_cast<lua_Integer>(static_cast<int>(s.damageLevel)));
    lua_setfield(L, t, "damage_level");
    lua_pushboolean(L, s.dead ? 1 : 0);
    lua_setfield(L, t, "dead");
    lua_pushboolean(L, s.playerOwned ? 1 : 0);
    lua_setfield(L, t, "player_owned");
    // Without this a script cannot tell friend from foe: `detected_contacts()` reports each contact's
    // faction, but a script had no way to learn its OWN, so `c.faction ~= state.faction` compared
    // against nil and every contact — friendly ones included — looked hostile (#694 found this by
    // executing the documented example, which is the entire reason the docs are now a test).
    lua_pushinteger(L, static_cast<lua_Integer>(s.factionIndex));
    lua_setfield(L, t, "faction");
    lua_pushinteger(L, static_cast<lua_Integer>(s.ownerId));
    lua_setfield(L, t, "owner_id");
    lua_pushinteger(L, static_cast<lua_Integer>(s.typeIndex));
    lua_setfield(L, t, "type_index");
}

static float readFloatField(lua_State* L, int idx, const char* field) {
    lua_getfield(L, idx, field);
    float v = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return v;
}

static bool readBoolField(lua_State* L, int idx, const char* field) {
    lua_getfield(L, idx, field);
    bool v = (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return v;
}

// ---------------------------------------------------------------------------
// guidance.* C closures (pure math)
// ---------------------------------------------------------------------------

// heading_error(quat, own_pos, target_pos, [radius_m]) — radius defaults to Earth.
static int guidanceHeadingError(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE); // quat
    luaL_checktype(L, 2, LUA_TTABLE); // own_pos
    luaL_checktype(L, 3, LUA_TTABLE); // target_pos
    float quat[4];
    double own[3];
    double tgt[3];
    readQuat(L, 1, quat);
    readVec3(L, 2, own);
    readVec3(L, 3, tgt);
    const double R = luaL_optnumber(L, 4, fl::kEarthRadiusM);
    lua_pushnumber(L, static_cast<double>(fl::ai::horizontalHeadingError(quat, own, tgt, R)));
    return 1;
}

// pitch_error_from_alt(quat, own_pos, alt_error_m, [radius_m]) — radius defaults to Earth.
static int guidancePitchErrorFromAlt(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE); // quat
    luaL_checktype(L, 2, LUA_TTABLE); // own_pos
    float altError = static_cast<float>(luaL_checknumber(L, 3));
    float quat[4];
    double own[3];
    readQuat(L, 1, quat);
    readVec3(L, 2, own);
    const double R = luaL_optnumber(L, 4, fl::kEarthRadiusM);
    lua_pushnumber(L, static_cast<double>(fl::ai::pitchErrorFromAlt(quat, own, altError, R)));
    return 1;
}

static int guidanceBankToTurnAileron(lua_State* L) {
    float herr = static_cast<float>(luaL_checknumber(L, 1));
    lua_pushnumber(L, static_cast<double>(fl::ai::bankToTurnAileron(herr)));
    return 1;
}

// turn_aileron(quat, own_pos, heading_error_rad, [radius_m], [max_bank_rad]) — the attitude-closed
// turn law (#1143). Aileron commands a roll RATE, so bank_to_turn_aileron above winds the roll up
// for as long as the heading error survives; every engine controller that flew a sustained turn on
// it reached ~180 deg of bank within 90 s. This one closes on the aircraft's own bank and stops.
static int guidanceTurnAileron(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE); // quat
    luaL_checktype(L, 2, LUA_TTABLE); // own_pos
    const float herr = static_cast<float>(luaL_checknumber(L, 3));
    float quat[4];
    double own[3];
    readQuat(L, 1, quat);
    readVec3(L, 2, own);
    const double R = luaL_optnumber(L, 4, fl::kEarthRadiusM);
    const float maxBank = static_cast<float>(luaL_optnumber(L, 5, static_cast<double>(fl::ai::kNavBankRad)));
    lua_pushnumber(L, static_cast<double>(fl::ai::bankToTurnAileron(quat, own, herr, R, maxBank)));
    return 1;
}

// sideslip(quat, vel) — the angle between where the aircraft points and where it is going.
static int guidanceSideslip(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE); // quat
    luaL_checktype(L, 2, LUA_TTABLE); // vel
    float quat[4];
    double vel[3];
    readQuat(L, 1, quat);
    readVec3(L, 2, vel);
    const float velF[3] = {static_cast<float>(vel[0]), static_cast<float>(vel[1]), static_cast<float>(vel[2])};
    lua_pushnumber(L, static_cast<double>(fl::ai::sideslipOf(quat, velF)));
    return 1;
}

// pitch_of(quat, own_pos, [radius_m]) — pitch attitude [rad] relative to the LOCAL horizon, so it is
// correct anywhere on the sphere rather than only near the world origin (#1196).
//
// It exists because elevator_for_altitude_hold's damping term needs a pitch RATE, and a script had
// no way to observe pitch at all: EntityState carries world velocity and an orientation quaternion
// but no body angular rates, so a controller differentiates pitch across its own sample interval —
// which it cannot do without being able to read pitch. Every engine-side controller does exactly
// this; the seam simply never exposed the first half.
static int guidancePitchOf(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE); // quat
    luaL_checktype(L, 2, LUA_TTABLE); // own_pos
    float quat[4];
    double own[3];
    readQuat(L, 1, quat);
    readVec3(L, 2, own);
    const double R = luaL_optnumber(L, 3, fl::kEarthRadiusM);
    lua_pushnumber(L, static_cast<double>(fl::pitchOf(quat, glm::dvec3(own[0], own[1], own[2]), R)));
    return 1;
}

// rudder_to_coordinate(sideslip_rad) — rudder that nulls a skid. THIS is turn coordination;
// coordinated_rudder(aileron) commands nothing in a steady turn, where the aileron is already zero.
static int guidanceRudderToCoordinate(lua_State* L) {
    const float beta = static_cast<float>(luaL_checknumber(L, 1));
    lua_pushnumber(L, static_cast<double>(fl::ai::rudderToCoordinate(beta)));
    return 1;
}

// elevator_for_altitude_hold(quat, own_pos, vel, target_alt_m, [radius_m, [max_aoa_rad,
// [pitch_rate_rad_s]]]) — altitude hold closed on CLIMB RATE. pitch_error_from_alt commands a pitch
// attitude and cannot tell "nose up" from "climbing": an aircraft mushing nose-high while descending
// satisfies it completely (#1141). max_aoa_rad sizes the loop to the airframe (#1186): the default
// serves a fighter, and a heavy aircraft must widen it or the bounded elevator can never reach the
// trim its level flight needs.
//
// pitch_rate_rad_s is the inner-loop damping (#1196). This binding used to pin it to zero with no
// way to supply it, so every scripted controller ran the loop WITHOUT the term the primitive's own
// documentation calls "not optional in practice" — while the engine's C++ controllers all passed it.
// Content flies the heavy aircraft this cascade was sized for, so content is precisely who needed
// it. Like the C++ side, a caller differentiates pitch across its own sample interval: EntityState
// carries world velocity but no body angular rates, on either side of the seam.
static int guidanceElevatorForAltitudeHold(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE); // quat
    luaL_checktype(L, 2, LUA_TTABLE); // own_pos
    luaL_checktype(L, 3, LUA_TTABLE); // vel
    const float targetAlt = static_cast<float>(luaL_checknumber(L, 4));
    float quat[4];
    double own[3];
    double vel[3];
    readQuat(L, 1, quat);
    readVec3(L, 2, own);
    readVec3(L, 3, vel);
    const float velF[3] = {static_cast<float>(vel[0]), static_cast<float>(vel[1]), static_cast<float>(vel[2])};
    const double R = luaL_optnumber(L, 5, fl::kEarthRadiusM);
    const float maxAoa = static_cast<float>(luaL_optnumber(L, 6, static_cast<double>(fl::ai::kDefaultMaxAoaRad)));
    const float pitchRate = static_cast<float>(luaL_optnumber(L, 7, 0.0));
    lua_pushnumber(
        L, static_cast<double>(fl::ai::elevatorForAltitudeHold(quat, own, velF, targetAlt, R, pitchRate, maxAoa)));
    return 1;
}

static int guidanceCoordinatedRudder(lua_State* L) {
    float aileron = static_cast<float>(luaL_checknumber(L, 1));
    lua_pushnumber(L, static_cast<double>(fl::ai::coordinatedRudder(aileron)));
    return 1;
}

static int guidanceElevatorFromPitchError(lua_State* L) {
    float perr = static_cast<float>(luaL_checknumber(L, 1));
    lua_pushnumber(L, static_cast<double>(fl::ai::elevatorFromPitchError(perr)));
    return 1;
}

static int guidanceBodyForward(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    float quat[4];
    readQuat(L, 1, quat);
    glm::vec3 fwd = fl::ai::bodyForward(quat);
    lua_newtable(L);
    lua_pushnumber(L, static_cast<double>(fwd.x));
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, static_cast<double>(fwd.y));
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, static_cast<double>(fwd.z));
    lua_setfield(L, -2, "z");
    return 1;
}

// ---------------------------------------------------------------------------
// nearby_entities(cx, cz, radius_m) → array of {idx, pos={x,y,z}}
// Upvalue 1: LuaController::Impl* (lightuserdata)
// ---------------------------------------------------------------------------

static int luaNearbyEntities(lua_State* L) {
    LuaController::Impl* impl = static_cast<LuaController::Impl*>(lua_touserdata(L, lua_upvalueindex(1)));
    double cx = luaL_checknumber(L, 1);
    double cz = luaL_checknumber(L, 2);
    double radius = luaL_checknumber(L, 3);

    lua_newtable(L);
    int resultTable = lua_gettop(L);

    if (impl->currentCtx && impl->currentCtx->si) {
        double center[3] = {cx, 0.0, cz};
        lua_Integer n = 1;
        impl->currentCtx->si->queryRadius(center, radius, [L, resultTable, &n](uint32_t idx, const double* pos) {
            lua_newtable(L);
            lua_pushinteger(L, static_cast<lua_Integer>(idx));
            lua_setfield(L, -2, "idx");
            lua_newtable(L);
            lua_pushnumber(L, pos[0]);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, pos[1]);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, pos[2]);
            lua_setfield(L, -2, "z");
            lua_setfield(L, -2, "pos");
            lua_rawseti(L, resultTable, n++);
        });
    }

    return 1;
}

// ---------------------------------------------------------------------------
// get_entity(idx) → state table or nil
// Upvalue 1: LuaController::Impl* (lightuserdata)
// ---------------------------------------------------------------------------

static int luaGetEntity(lua_State* L) {
    LuaController::Impl* impl = static_cast<LuaController::Impl*>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_Integer idx = luaL_checkinteger(L, 1);

    if (!impl->entityManager) {
        lua_pushnil(L);
        return 1;
    }

    const fl::EntityState* found = nullptr;
    impl->entityManager->forEach([&found, idx](const fl::EntityState& s) {
        if (!found && !s.dead && s.id.index == static_cast<uint32_t>(idx))
            found = &s;
    });

    if (!found) {
        lua_pushnil(L);
        return 1;
    }

    pushEntityState(L, *found);
    return 1;
}

// ---------------------------------------------------------------------------
// detected_contacts() → array of {idx, state, pos, vel, age_s, reacted, sensor_types}
// Upvalue 1: LuaController::Impl* (lightuserdata)
//
// THE HONEST VIEW. nearby_entities() is a raw radius query over ground truth — it sees through
// terrain, through the back of the aircraft's head, and at any range. This returns only what the
// entity has actually DETECTED, with LAST-KNOWN position and velocity: a coasting contact reports
// where the target WAS, not where it is.
//
// Returns {} when sensing was not evaluated (ctx.contacts == nullptr — headless callers and tests),
// so every existing script keeps working unchanged. Note that is different from an EMPTY table,
// which means the sensors ran and found nothing.
// ---------------------------------------------------------------------------

static const char* contactStateName(fl::sensor::ContactState s) {
    switch (s) {
    case fl::sensor::ContactState::Detected:
        return "detected";
    case fl::sensor::ContactState::Locked:
        return "locked";
    case fl::sensor::ContactState::Coasting:
        return "coasting";
    case fl::sensor::ContactState::Lost:
        break;
    }
    return "lost"; // never stored in a table; a Lost contact is not in it at all
}

static int luaDetectedContacts(lua_State* L) {
    LuaController::Impl* impl = static_cast<LuaController::Impl*>(lua_touserdata(L, lua_upvalueindex(1)));

    lua_newtable(L);
    const int resultTable = lua_gettop(L);

    if (!impl->currentCtx || !impl->currentCtx->contacts)
        return 1; // sensing not evaluated — an empty table, and existing scripts carry on

    lua_Integer n = 1;
    for (const fl::sensor::Contact& c : *impl->currentCtx->contacts) {
        lua_newtable(L);

        lua_pushinteger(L, static_cast<lua_Integer>(c.id.index));
        lua_setfield(L, -2, "idx");

        lua_pushstring(L, contactStateName(c.state));
        lua_setfield(L, -2, "state");

        // LAST-KNOWN, not live. A script that wants honest behavior steers at these.
        lua_newtable(L);
        lua_pushnumber(L, c.lastKnownPos[0]);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, c.lastKnownPos[1]);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, c.lastKnownPos[2]);
        lua_setfield(L, -2, "z");
        lua_setfield(L, -2, "pos");

        lua_newtable(L);
        lua_pushnumber(L, static_cast<double>(c.lastKnownVel[0]));
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, static_cast<double>(c.lastKnownVel[1]));
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, static_cast<double>(c.lastKnownVel[2]));
        lua_setfield(L, -2, "z");
        lua_setfield(L, -2, "vel");

        // How stale this is. 0 while the contact is actually being seen; it grows while coasting,
        // which is precisely when a script should stop trusting `pos`.
        const uint64_t ticksSince = impl->currentTick > c.lastSeenTick ? impl->currentTick - c.lastSeenTick : 0;
        lua_pushnumber(L, static_cast<double>(ticksSince) * impl->currentDt);
        lua_setfield(L, -2, "age_s");

        lua_pushboolean(L, c.reacted ? 1 : 0);
        lua_setfield(L, -2, "reacted");

        lua_pushinteger(L, static_cast<lua_Integer>(c.factionIndex));
        lua_setfield(L, -2, "faction");

        // Which KINDS of sensor hold it. "He has me on radar" and "he can see me" are different
        // tactical facts, and a script is entitled to tell them apart.
        lua_newtable(L);
        lua_Integer si = 1;
        const std::pair<fl::sensor::SensorType, const char*> kTypes[] = {
            {fl::sensor::SensorType::Visual, "visual"},
            {fl::sensor::SensorType::Ir, "ir"},
            {fl::sensor::SensorType::Radar, "radar"},
            {fl::sensor::SensorType::Laser, "laser"},
        };
        for (const auto& [type, name] : kTypes) {
            if (fl::sensor::holdsSensorType(c.sensorTypeMask, type)) {
                lua_pushstring(L, name);
                lua_rawseti(L, -2, si++);
            }
        }
        lua_setfield(L, -2, "sensor_types");

        lua_rawseti(L, resultTable, n++);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// world.* module (#413) — engine integration routed through the host WorldApi seam.
// Upvalue 1: LuaController::Impl* (lightuserdata). An unset hook is a safe no-op.
// ---------------------------------------------------------------------------

static LuaController::Impl* worldImpl(lua_State* L) {
    return static_cast<LuaController::Impl*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// world.spawn(type_id, pos, heading, [side]) -> entity idx (or -1 on failure / no host)
static int luaWorldSpawn(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const char* typeId = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE); // pos {x,y,z}
    double pos[3];
    readVec3(L, 2, pos);
    const float heading = static_cast<float>(luaL_checknumber(L, 3));
    const char* side = luaL_optstring(L, 4, "");
    int idx = -1;
    if (impl->worldApi && impl->worldApi->spawn)
        idx = impl->worldApi->spawn(typeId, {pos[0], pos[1], pos[2]}, heading, side);
    lua_pushinteger(L, idx);
    return 1;
}

// world.despawn(entity_idx)
static int luaWorldDespawn(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const lua_Integer idx = luaL_checkinteger(L, 1);
    if (impl->worldApi && impl->worldApi->despawn)
        impl->worldApi->despawn(static_cast<int>(idx));
    return 0;
}

// world.set_relationship(faction_a, faction_b, rel)  rel = friendly|neutral|hostile
static int luaWorldSetRelationship(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const char* a = luaL_checkstring(L, 1);
    const char* b = luaL_checkstring(L, 2);
    const char* rel = luaL_checkstring(L, 3);
    if (impl->worldApi && impl->worldApi->setRelationship)
        impl->worldApi->setRelationship(a, b, rel);
    return 0;
}

// world.set_music_state(state)  state = menu|patrol|combat|success|debrief
static int luaWorldSetMusicState(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const char* state = luaL_checkstring(L, 1);
    if (impl->worldApi && impl->worldApi->setMusicState)
        impl->worldApi->setMusicState(state);
    return 0;
}

// world.set_alert_level(faction_id, level)  level = peacetime|elevated|conflict|war_state (#162)
static int luaWorldSetAlertLevel(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const char* faction = luaL_checkstring(L, 1);
    const char* level = luaL_checkstring(L, 2);
    if (impl->worldApi && impl->worldApi->setAlertLevel)
        impl->worldApi->setAlertLevel(faction, level);
    return 0;
}

// world.get_alert_level(faction_id) -> string (#162). "peacetime" with no host hook, so a script can
// branch on the result without checking whether the server has an alert system.
static int luaWorldGetAlertLevel(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const char* faction = luaL_checkstring(L, 1);
    std::string level = "peacetime";
    if (impl->worldApi && impl->worldApi->getAlertLevel)
        level = impl->worldApi->getAlertLevel(faction);
    lua_pushstring(L, level.c_str());
    return 1;
}

// world.get_zone_stage(entity_idx, zone_id) -> string (#162)
static int luaWorldGetZoneStage(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const int idx = static_cast<int>(luaL_checkinteger(L, 1));
    const char* zoneId = luaL_checkstring(L, 2);
    std::string stage = "clean";
    if (impl->worldApi && impl->worldApi->getZoneStage && idx >= 0)
        stage = impl->worldApi->getZoneStage(idx, zoneId);
    lua_pushstring(L, stage.c_str());
    return 1;
}

// world.is_in_zone(entity_idx, zone_id) -> bool (#162)
static int luaWorldIsInZone(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const int idx = static_cast<int>(luaL_checkinteger(L, 1));
    const char* zoneId = luaL_checkstring(L, 2);
    bool inside = false;
    if (impl->worldApi && impl->worldApi->isInZone && idx >= 0)
        inside = impl->worldApi->isInZone(idx, zoneId);
    lua_pushboolean(L, inside ? 1 : 0);
    return 1;
}

static int luaWorldMissionSuccess(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    if (impl->worldApi && impl->worldApi->setMissionOutcome)
        impl->worldApi->setMissionOutcome(true);
    return 0;
}

static int luaWorldMissionFailure(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    if (impl->worldApi && impl->worldApi->setMissionOutcome)
        impl->worldApi->setMissionOutcome(false);
    return 0;
}

// world.score_objective(faction, count)  -- award `count` (default 1) objectives to team `faction` (#1000)
static int luaWorldScoreObjective(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const int faction = static_cast<int>(luaL_checkinteger(L, 1));
    const int count = lua_isnoneornil(L, 2) ? 1 : static_cast<int>(luaL_checkinteger(L, 2));
    if (impl->worldApi && impl->worldApi->scoreObjective && faction >= 0)
        impl->worldApi->scoreObjective(faction, count);
    return 0;
}

// world.get_elapsed_time() -> seconds since this controller started
static int luaWorldGetElapsedTime(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    lua_pushnumber(L, impl->elapsedS);
    return 1;
}

// world.on_trigger(predicate_fn, callback_fn) -- fires callback once, the first tick predicate is true
static int luaWorldOnTrigger(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 1);
    const int predRef = luaL_ref(L, LUA_REGISTRYINDEX);
    impl->triggers.push_back({predRef, cbRef});
    return 0;
}

// world.timer(seconds, callback_fn) -- fires callback once after N sim-seconds
static int luaWorldTimer(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    impl->timers.push_back({impl->elapsedS + seconds, cbRef});
    return 0;
}

// ---------------------------------------------------------------------------
// Haptics (#128) — bare globals rumble / rumble_triggers / stop_rumble, routed through the WorldApi
// seam. Sandbox guards live HERE (not in the host): a script never names a gamepad, the intensities are
// clamped to [0,1], and the duration is capped so an untrusted mod cannot lock rumble on indefinitely.
// ---------------------------------------------------------------------------

// The longest single rumble a script can request. A mod would have to actively re-issue to sustain it,
// and stop_rumble is always available — so rumble cannot be latched on and forgotten.
static constexpr uint32_t kMaxRumbleMs = 5000;

static float clamp01(double v) {
    if (v < 0.0)
        return 0.f;
    if (v > 1.0)
        return 1.f;
    return static_cast<float>(v);
}

static uint32_t clampRumbleMs(double v) {
    if (v < 0.0)
        return 0u;
    if (v > static_cast<double>(kMaxRumbleMs))
        return kMaxRumbleMs;
    return static_cast<uint32_t>(v);
}

// rumble(low_freq, high_freq, duration_ms)
static int luaRumble(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const float low = clamp01(luaL_checknumber(L, 1));
    const float high = clamp01(luaL_checknumber(L, 2));
    const uint32_t dur = clampRumbleMs(luaL_checknumber(L, 3));
    if (impl->worldApi && impl->worldApi->rumble)
        impl->worldApi->rumble(low, high, dur);
    return 0;
}

// rumble_triggers(left, right, duration_ms)
static int luaRumbleTriggers(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const float left = clamp01(luaL_checknumber(L, 1));
    const float right = clamp01(luaL_checknumber(L, 2));
    const uint32_t dur = clampRumbleMs(luaL_checknumber(L, 3));
    if (impl->worldApi && impl->worldApi->rumbleTriggers)
        impl->worldApi->rumbleTriggers(left, right, dur);
    return 0;
}

// stop_rumble()
static int luaStopRumble(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    if (impl->worldApi && impl->worldApi->stopRumble)
        impl->worldApi->stopRumble();
    return 0;
}

// ── atc.* module (#705) ──────────────────────────────────────────────────────
// Self-service bindings for the entity this controller flies (clearance/request_*/inbound) plus the
// airport-addressed scramble/hold. All nil/false when no AtcService is wired. The service is
// thread-safe, so these are safe to call from the parallel AI pass.

// Optional trailing airport-id argument (arg `n`), or "" (nearest).
static std::string atcFacilityArg(lua_State* L, int n) {
    if (lua_isstring(L, n))
        return lua_tostring(L, n);
    return {};
}

// atc.clearance() -> clearance-state string ("none" when unknown / no service)
static int luaAtcClearance(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    fl::atc::ClearanceState s = fl::atc::ClearanceState::None;
    if (impl->atcService)
        s = impl->atcService->clearanceState(impl->currentSelfId);
    lua_pushstring(L, fl::atc::clearanceStateName(s));
    return 1;
}

// atc.request_takeoff([airport_id]) -> bool
static int luaAtcRequestTakeoff(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    if (!impl->atcService) {
        lua_pushboolean(L, 0);
        return 1;
    }
    impl->atcService->requestTakeoff(impl->currentSelfId, atcFacilityArg(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

// atc.request_landing([airport_id]) -> bool
static int luaAtcRequestLanding(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    if (!impl->atcService) {
        lua_pushboolean(L, 0);
        return 1;
    }
    impl->atcService->requestLanding(impl->currentSelfId, atcFacilityArg(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

// atc.inbound([airport_id]) -> bool
static int luaAtcInbound(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    if (!impl->atcService) {
        lua_pushboolean(L, 0);
        return 1;
    }
    impl->atcService->declareInbound(impl->currentSelfId, atcFacilityArg(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

// atc.scramble(airport_id, type_id, count) -> bool
static int luaAtcScramble(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const char* airport = luaL_checkstring(L, 1);
    const char* type = luaL_checkstring(L, 2);
    const int count = static_cast<int>(luaL_optinteger(L, 3, 1));
    bool ok = false;
    if (impl->atcService)
        ok = impl->atcService->scramble(airport, type, count);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// atc.hold(airport_id, on) -> bool
static int luaAtcHold(lua_State* L) {
    LuaController::Impl* impl = worldImpl(L);
    const char* airport = luaL_checkstring(L, 1);
    const bool hold = lua_toboolean(L, 2) != 0;
    if (!impl->atcService) {
        lua_pushboolean(L, 0);
        return 1;
    }
    impl->atcService->holdDepartures(airport, hold);
    lua_pushboolean(L, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// API registration
// ---------------------------------------------------------------------------

static void registerAtcModule(lua_State* L, LuaController::Impl* impl) {
    static const luaL_Reg kFuncs[] = {
        {"clearance", luaAtcClearance},
        {"request_takeoff", luaAtcRequestTakeoff},
        {"request_landing", luaAtcRequestLanding},
        {"inbound", luaAtcInbound},
        {"scramble", luaAtcScramble},
        {"hold", luaAtcHold},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    for (const luaL_Reg* r = kFuncs; r->name; ++r) {
        lua_pushlightuserdata(L, impl);
        lua_pushcclosure(L, r->func, 1);
        lua_setfield(L, -2, r->name);
    }
    lua_setglobal(L, "atc");
}

static void registerWorldModule(lua_State* L, LuaController::Impl* impl) {
    static const luaL_Reg kFuncs[] = {
        {"spawn", luaWorldSpawn},
        {"despawn", luaWorldDespawn},
        {"set_relationship", luaWorldSetRelationship},
        {"set_music_state", luaWorldSetMusicState},
        {"set_alert_level", luaWorldSetAlertLevel},
        {"get_alert_level", luaWorldGetAlertLevel},
        {"get_zone_stage", luaWorldGetZoneStage},
        {"is_in_zone", luaWorldIsInZone},
        {"mission_success", luaWorldMissionSuccess},
        {"mission_failure", luaWorldMissionFailure},
        {"score_objective", luaWorldScoreObjective},
        {"get_elapsed_time", luaWorldGetElapsedTime},
        {"on_trigger", luaWorldOnTrigger},
        {"timer", luaWorldTimer},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    // Each function is a closure over the Impl* (lightuserdata upvalue), like the spatial funcs.
    for (const luaL_Reg* r = kFuncs; r->name; ++r) {
        lua_pushlightuserdata(L, impl);
        lua_pushcclosure(L, r->func, 1);
        lua_setfield(L, -2, r->name);
    }
    lua_setglobal(L, "world");

    // Haptics (#128) are bare globals (no gamepad id exposed to scripts), not under world.*.
    static const luaL_Reg kHaptics[] = {
        {"rumble", luaRumble},
        {"rumble_triggers", luaRumbleTriggers},
        {"stop_rumble", luaStopRumble},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* r = kHaptics; r->name; ++r) {
        lua_pushlightuserdata(L, impl);
        lua_pushcclosure(L, r->func, 1);
        lua_setglobal(L, r->name);
    }
}

static void registerGuidanceModule(lua_State* L) {
    static const luaL_Reg kFuncs[] = {
        {"heading_error", guidanceHeadingError},
        {"pitch_error_from_alt", guidancePitchErrorFromAlt},
        {"bank_to_turn_aileron", guidanceBankToTurnAileron},
        {"turn_aileron", guidanceTurnAileron},
        {"coordinated_rudder", guidanceCoordinatedRudder},
        {"sideslip", guidanceSideslip},
        {"rudder_to_coordinate", guidanceRudderToCoordinate},
        {"elevator_for_altitude_hold", guidanceElevatorForAltitudeHold},
        {"elevator_from_pitch_error", guidanceElevatorFromPitchError},
        {"body_forward", guidanceBodyForward},
        {"pitch_of", guidancePitchOf},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    luaL_setfuncs(L, kFuncs, 0);
    lua_setglobal(L, "guidance");
}

static void registerSpatialFuncs(lua_State* L, LuaController::Impl* impl) {
    lua_pushlightuserdata(L, impl);
    lua_pushcclosure(L, luaNearbyEntities, 1);
    lua_setglobal(L, "nearby_entities");

    lua_pushlightuserdata(L, impl);
    lua_pushcclosure(L, luaGetEntity, 1);
    lua_setglobal(L, "get_entity");

    lua_pushlightuserdata(L, impl);
    lua_pushcclosure(L, luaDetectedContacts, 1);
    lua_setglobal(L, "detected_contacts");
}

// ---------------------------------------------------------------------------
// LuaController
// ---------------------------------------------------------------------------

LuaController::LuaController(std::string_view scriptSource, ScriptPackSource pack,
                             const fl::EntityManager* entityManager, const fl::WorldApi* worldApi,
                             fl::atc::AtcService* atcService)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->entityManager = entityManager;
    m_impl->worldApi = worldApi;
    m_impl->atcService = atcService;

    m_impl->sandbox = LuaSandbox::create(std::move(pack));
    if (!m_impl->sandbox) {
        m_impl->lastError = "failed to create Lua sandbox";
        return;
    }

    lua_State* L = m_impl->sandbox->luaState();
    registerGuidanceModule(L);
    registerSpatialFuncs(L, m_impl.get());
    registerWorldModule(L, m_impl.get());
    registerAtcModule(L, m_impl.get());

    if (!m_impl->sandbox->loadScript(scriptSource)) {
        m_impl->lastError = m_impl->sandbox->lastError();
        return;
    }

    // Coroutine control-flow (#412): if the script defines `ai_main`, drive it as a coroutine resumed
    // once per tick rather than calling compute_control. Create the thread now and keep it alive with a
    // registry ref; the initial resume (which starts ai_main) happens on the first sample(). A coroutine
    // shares the sandbox's global environment, so guidance.* / nearby_entities / detected_contacts and
    // the deny-list all apply inside ai_main exactly as they do inside compute_control.
    lua_getglobal(L, "ai_main");
    const bool hasAiMain = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (hasAiMain) {
        lua_State* co = lua_newthread(L);                      // pushes the new thread
        m_impl->coroutineRef = luaL_ref(L, LUA_REGISTRYINDEX); // pops it, anchors against GC
        m_impl->coroutine = co;
        m_impl->useCoroutine = true;
    }

    m_impl->valid = true;
}

LuaController::~LuaController() = default;

bool LuaController::isValid() const {
    return m_impl->valid;
}

const std::string& LuaController::lastError() const {
    return m_impl->lastError;
}

// Map the control table at stack index `idx` (on state `L`) onto a ControlInput. Shared by the
// compute_control return value and the coroutine's yielded value so the two entry points cannot drift.
// A non-table `idx` yields neutral. Leaves the stack unchanged (only lua_getfield + pop internally).
static fl::ControlInput readControlTable(lua_State* L, int idx) {
    fl::ControlInput ctrl{};
    if (!lua_istable(L, idx))
        return ctrl;
    ctrl.elevator = readFloatField(L, idx, "elevator");
    ctrl.aileron = readFloatField(L, idx, "aileron");
    ctrl.rudder = readFloatField(L, idx, "rudder");
    ctrl.throttle = readFloatField(L, idx, "throttle");
    ctrl.afterburner = readBoolField(L, idx, "afterburner");
    ctrl.speedbrake = readFloatField(L, idx, "speedbrake");
    ctrl.gear_down = readBoolField(L, idx, "gear_down");
    // Fire intent (#625) — the same seam players and C++ AI use. The value is an INTENT: the
    // server's FireControl validates station/ammo/rate/weapons-hold exactly as it does for a
    // player, so a hostile script holding the trigger forever gets what a trigger-holding player
    // gets. weapon_station is absolute (matches the wire semantics); absent field = keep.
    ctrl.trigger = readBoolField(L, idx, "trigger");
    ctrl.release = readBoolField(L, idx, "release");
    lua_getfield(L, idx, "weapon_station");
    if (lua_isnumber(L, -1)) {
        const lua_Integer st = lua_tointeger(L, -1);
        if (st >= 0 && st <= 254)
            ctrl.station = static_cast<uint8_t>(st);
    }
    lua_pop(L, 1);
    return ctrl;
}

// Coroutine control flow (#412): resume `ai_main` once, passing (state, tick, dt); its first yielded
// value is this tick's control table. A finished (or errored) coroutine leaves coroutineDead set and
// every subsequent tick is neutral — the same fail-safe as a compute_control error.
fl::ControlInput LuaController::sampleCoroutine(const fl::EntityState& state, uint64_t tick, double dt) {
    Impl& im = *m_impl;
    if (im.coroutineDead)
        return {};

    lua_State* co = im.coroutine;
    lua_State* main = im.sandbox->luaState();

    // Fresh thread: push ai_main so the first resume starts it below its args.
    if (!im.coroutineStarted) {
        lua_getglobal(co, "ai_main");
        im.coroutineStarted = true;
    }

    // Args become the initial ai_main() args on the first resume, and the return values of the
    // coroutine.yield(...) that suspended it on every later resume.
    pushEntityState(co, state);
    lua_pushnumber(co, static_cast<lua_Number>(tick));
    lua_pushnumber(co, dt);

    int nresults = 0;
    const int status = lua_resume(co, main, 3, &nresults);

    if (status == LUA_YIELD) {
        // Yielded values sit on co's stack; the first is the control table.
        fl::ControlInput ctrl{};
        if (nresults >= 1)
            ctrl = readControlTable(co, lua_gettop(co) - nresults + 1);
        lua_settop(co, 0); // discard the yielded values; the next resume repushes args
        return ctrl;
    }

    if (status == LUA_OK) {
        // ai_main returned: the behavior is finished. Neutral from here on.
        im.coroutineDead = true;
        lua_settop(co, 0);
        return {};
    }

    // Runtime error inside the coroutine — fail safe (neutral) and stop resuming it.
    const char* err = lua_tostring(co, -1);
    if (tick >= im.nextErrorLogTick) {
        std::fprintf(stderr, "[LUA WARN] ai_main error: %s\n", err ? err : "(unknown)");
        im.nextErrorLogTick = tick + 60;
    }
    im.coroutineDead = true;
    lua_settop(co, 0);
    return {};
}

// Evaluate the world.timer / world.on_trigger registrations (#413) once this tick, firing each
// callback at most once and removing it. Runs on the sandbox's main state with the world view already
// stashed, so a callback may itself call world.* / guidance.* etc. A predicate/callback error is
// logged (rate-limited) and the registration dropped — a broken trigger never wedges the tick.
static void evaluateWorldTriggers(LuaController::Impl& im, uint64_t tick) {
    if (im.timers.empty() && im.triggers.empty())
        return;
    lua_State* L = im.sandbox->luaState();

    // Timers: fire and drop any whose deadline has passed.
    for (std::size_t i = 0; i < im.timers.size();) {
        if (im.elapsedS + 1e-9 >= im.timers[i].fireAt) {
            const int cbRef = im.timers[i].cbRef;
            im.timers.erase(im.timers.begin() + static_cast<std::ptrdiff_t>(i));
            lua_rawgeti(L, LUA_REGISTRYINDEX, cbRef);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                if (tick >= im.nextErrorLogTick) {
                    std::fprintf(stderr, "[LUA WARN] world.timer callback error: %s\n",
                                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "(unknown)");
                    im.nextErrorLogTick = tick + 60;
                }
                lua_pop(L, 1);
            }
            luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
        } else {
            ++i;
        }
    }

    // Triggers: evaluate each predicate; on the first truthy result fire the callback and drop it.
    for (std::size_t i = 0; i < im.triggers.size();) {
        const LuaController::Impl::TriggerReg reg = im.triggers[i];
        lua_rawgeti(L, LUA_REGISTRYINDEX, reg.predRef);
        bool fired = false;
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            if (tick >= im.nextErrorLogTick) {
                std::fprintf(stderr, "[LUA WARN] world.on_trigger predicate error: %s\n",
                             lua_tostring(L, -1) ? lua_tostring(L, -1) : "(unknown)");
                im.nextErrorLogTick = tick + 60;
            }
            lua_pop(L, 1);
            fired = true; // drop a broken predicate rather than re-evaluate it forever
        } else {
            fired = (lua_toboolean(L, -1) != 0);
            lua_pop(L, 1);
        }
        if (!fired) {
            ++i;
            continue;
        }
        im.triggers.erase(im.triggers.begin() + static_cast<std::ptrdiff_t>(i));
        lua_rawgeti(L, LUA_REGISTRYINDEX, reg.cbRef);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            if (tick >= im.nextErrorLogTick) {
                std::fprintf(stderr, "[LUA WARN] world.on_trigger callback error: %s\n",
                             lua_tostring(L, -1) ? lua_tostring(L, -1) : "(unknown)");
                im.nextErrorLogTick = tick + 60;
            }
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, reg.predRef);
        luaL_unref(L, LUA_REGISTRYINDEX, reg.cbRef);
    }
}

fl::ControlInput LuaController::sample(const fl::EntityState& state, uint64_t tick, double dt,
                                       const fl::AiTickContext& ctx) {
    if (!m_impl->valid)
        return {};

    // Stash the world view for the C bindings (guidance.*/nearby_entities/detected_contacts) for the
    // duration of this tick's Lua execution, cleared on every exit path (coroutine path included).
    m_impl->currentCtx = &ctx;
    m_impl->currentTick = tick;
    m_impl->currentDt = dt;
    m_impl->currentSelfId = state.id; // for the atc.* self-service bindings (#705)
    m_impl->elapsedS += dt;

    // Fire due world.timer / world.on_trigger callbacks before computing control this tick (#413).
    evaluateWorldTriggers(*m_impl, tick);

    if (m_impl->useCoroutine) {
        fl::ControlInput ctrl = sampleCoroutine(state, tick, dt);
        m_impl->currentCtx = nullptr;
        return ctrl;
    }

    lua_State* L = m_impl->sandbox->luaState();

    // Push compute_control function.
    lua_getglobal(L, "compute_control");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (tick >= m_impl->nextErrorLogTick) {
            std::fprintf(stderr, "[LUA WARN] compute_control is not a function\n");
            m_impl->nextErrorLogTick = tick + 60;
        }
        m_impl->currentCtx = nullptr;
        return {};
    }

    // Push args: state table, tick, dt.
    pushEntityState(L, state);
    lua_pushnumber(L, static_cast<lua_Number>(tick));
    lua_pushnumber(L, dt);

    // Protected call: 3 args, 1 result.
    if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        if (tick >= m_impl->nextErrorLogTick) {
            std::fprintf(stderr, "[LUA WARN] compute_control error: %s\n", err ? err : "(unknown)");
            m_impl->nextErrorLogTick = tick + 60;
        }
        lua_pop(L, 1);
        m_impl->currentCtx = nullptr;
        return {};
    }

    // Read result table.
    if (!lua_istable(L, -1)) {
        if (tick >= m_impl->nextErrorLogTick) {
            std::fprintf(stderr, "[LUA WARN] compute_control did not return a table\n");
            m_impl->nextErrorLogTick = tick + 60;
        }
        lua_pop(L, 1);
        m_impl->currentCtx = nullptr;
        return {};
    }

    const fl::ControlInput ctrl = readControlTable(L, lua_gettop(L));
    lua_pop(L, 1); // the result table

    m_impl->currentCtx = nullptr;
    return ctrl;
}

} // namespace fl
