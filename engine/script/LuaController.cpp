// SPDX-License-Identifier: GPL-3.0-or-later
#include "script/LuaController.h"
#include "script/LuaSandbox.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "ai/Guidance.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "spatial/SpatialIndex.h"

#include <cstdint>
#include <cstdio>

namespace fl {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct LuaController::Impl {
    std::unique_ptr<LuaSandbox> sandbox;
    const fl::EntityManager* entityManager{nullptr};
    const fl::SpatialIndex* currentSi{nullptr};
    bool valid{false};
    std::string lastError;
    uint64_t nextErrorLogTick{0}; // rate-limit: log at most once per 60 ticks
};

// ---------------------------------------------------------------------------
// Stack helpers (no C++ objects with non-trivial dtors — longjmp-safe)
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
// guidance.* C closures (pure math, no C++ objects — longjmp-safe)
// ---------------------------------------------------------------------------

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
    lua_pushnumber(L, static_cast<double>(fl::ai::horizontalHeadingError(quat, own, tgt)));
    return 1;
}

static int guidancePitchErrorFromAlt(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE); // quat
    float altError = static_cast<float>(luaL_checknumber(L, 2));
    float quat[4];
    readQuat(L, 1, quat);
    lua_pushnumber(L, static_cast<double>(fl::ai::pitchErrorFromAlt(quat, altError)));
    return 1;
}

static int guidanceBankToTurnAileron(lua_State* L) {
    float herr = static_cast<float>(luaL_checknumber(L, 1));
    lua_pushnumber(L, static_cast<double>(fl::ai::bankToTurnAileron(herr)));
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

    if (impl->currentSi) {
        double center[3] = {cx, 0.0, cz};
        lua_Integer n = 1;
        impl->currentSi->queryRadius(center, radius, [L, resultTable, &n](uint32_t idx, const double* pos) {
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
// API registration
// ---------------------------------------------------------------------------

static void registerGuidanceModule(lua_State* L) {
    static const luaL_Reg kFuncs[] = {
        {"heading_error", guidanceHeadingError},
        {"pitch_error_from_alt", guidancePitchErrorFromAlt},
        {"bank_to_turn_aileron", guidanceBankToTurnAileron},
        {"coordinated_rudder", guidanceCoordinatedRudder},
        {"elevator_from_pitch_error", guidanceElevatorFromPitchError},
        {"body_forward", guidanceBodyForward},
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
}

// ---------------------------------------------------------------------------
// LuaController
// ---------------------------------------------------------------------------

LuaController::LuaController(std::string_view scriptSource, std::string packRootDir,
                             const fl::EntityManager* entityManager)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->entityManager = entityManager;

    m_impl->sandbox = LuaSandbox::create(std::move(packRootDir));
    if (!m_impl->sandbox) {
        m_impl->lastError = "failed to create Lua sandbox";
        return;
    }

    lua_State* L = m_impl->sandbox->luaState();
    registerGuidanceModule(L);
    registerSpatialFuncs(L, m_impl.get());

    if (!m_impl->sandbox->loadScript(scriptSource)) {
        m_impl->lastError = m_impl->sandbox->lastError();
        return;
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

fl::ControlInput LuaController::sample(const fl::EntityState& state, uint64_t tick, double dt,
                                       const fl::SpatialIndex* si) {
    if (!m_impl->valid)
        return {};

    lua_State* L = m_impl->sandbox->luaState();
    m_impl->currentSi = si;

    // Push compute_control function.
    lua_getglobal(L, "compute_control");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (tick >= m_impl->nextErrorLogTick) {
            std::fprintf(stderr, "[LUA WARN] compute_control is not a function\n");
            m_impl->nextErrorLogTick = tick + 60;
        }
        m_impl->currentSi = nullptr;
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
        m_impl->currentSi = nullptr;
        return {};
    }

    // Read result table.
    if (!lua_istable(L, -1)) {
        if (tick >= m_impl->nextErrorLogTick) {
            std::fprintf(stderr, "[LUA WARN] compute_control did not return a table\n");
            m_impl->nextErrorLogTick = tick + 60;
        }
        lua_pop(L, 1);
        m_impl->currentSi = nullptr;
        return {};
    }

    int resultIdx = lua_gettop(L);
    fl::ControlInput ctrl{};
    ctrl.elevator = readFloatField(L, resultIdx, "elevator");
    ctrl.aileron = readFloatField(L, resultIdx, "aileron");
    ctrl.rudder = readFloatField(L, resultIdx, "rudder");
    ctrl.throttle = readFloatField(L, resultIdx, "throttle");
    ctrl.afterburner = readBoolField(L, resultIdx, "afterburner");
    ctrl.speedbrake = readFloatField(L, resultIdx, "speedbrake");
    ctrl.gear_down = readBoolField(L, resultIdx, "gear_down");
    lua_pop(L, 1);

    m_impl->currentSi = nullptr;
    return ctrl;
}

} // namespace fl
