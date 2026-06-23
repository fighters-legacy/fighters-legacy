// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "script/LuaController.h"
#include "spatial/SpatialIndex.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fl;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct NullLoggerL : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

static fl::EntityState makeState(double px = 0.0, double py = 600.0, double pz = 0.0, float hp = 100.f,
                                 float maxHp = 100.f) {
    fl::EntityState s{};
    s.id = {1, 1};
    s.transform.pos[0] = px;
    s.transform.pos[1] = py;
    s.transform.pos[2] = pz;
    s.transform.vel[0] = 10.f;
    s.transform.vel[1] = 0.f;
    s.transform.vel[2] = 5.f;
    s.transform.quat[0] = 0.f;
    s.transform.quat[1] = 0.f;
    s.transform.quat[2] = 0.f;
    s.transform.quat[3] = 1.f; // identity
    s.hp = hp;
    s.maxHp = maxHp;
    s.typeIndex = 7;
    s.ownerId = 3;
    return s;
}

static std::unique_ptr<LuaController> makeCtrl(const char* src) {
    auto c = std::make_unique<LuaController>(src, "");
    return c;
}

// ---------------------------------------------------------------------------
// Control output
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: neutral ControlInput from minimal script") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {} end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.elevator == Catch::Approx(0.f));
    CHECK(ctrl.throttle == Catch::Approx(0.f));
    CHECK_FALSE(ctrl.afterburner);
}

TEST_CASE("LuaController: throttle returned from script") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {throttle=0.75} end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.75f).epsilon(0.001f));
}

TEST_CASE("LuaController: elevator aileron rudder from script") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {elevator=0.5,aileron=-0.3,rudder=0.1} end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.elevator == Catch::Approx(0.5f).epsilon(0.001f));
    CHECK(ctrl.aileron == Catch::Approx(-0.3f).epsilon(0.001f));
    CHECK(ctrl.rudder == Catch::Approx(0.1f).epsilon(0.001f));
}

TEST_CASE("LuaController: afterburner bool returned from script") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {throttle=1.0,afterburner=true} end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
    CHECK(ctrl.afterburner);
}

TEST_CASE("LuaController: speedbrake returned from script") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {speedbrake=0.5} end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.speedbrake == Catch::Approx(0.5f).epsilon(0.001f));
}

TEST_CASE("LuaController: gear_down returned from script") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {gear_down=true} end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.gear_down);
}

// ---------------------------------------------------------------------------
// State table access
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: state pos accessible") {
    // throttle=1 if pos.y > 500 (makeState default py=600)
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  if s.pos.y > 500 then return {throttle=1.0} else return {} end\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(0.0, 600.0, 0.0), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: state vel accessible") {
    // vel.x = 10.f → throttle=1 when vel.x > 5
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  if s.vel.x > 5 then return {throttle=1.0} else return {} end\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: state quat accessible") {
    // identity quat has w=1; throttle=1 when quat.w > 0.9
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  if s.quat.w > 0.9 then return {throttle=1.0} else return {} end\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: state hp and max_hp accessible") {
    // hp=100, max_hp=100 → elevator=1 when hp==max_hp
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  if s.hp == s.max_hp then return {elevator=1.0} else return {} end\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(0.0, 600.0, 0.0, 100.f, 100.f), 0, 1.0 / 60.0);
    CHECK(ctrl.elevator == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: state damage_level and dead accessible") {
    // damage_level=0 (Intact), dead=false → throttle=1
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  if s.damage_level == 0 and not s.dead then return {throttle=1.0}\n"
                      "  else return {} end\n"
                      "end");
    REQUIRE(c->isValid());
    fl::EntityState st = makeState();
    st.damageLevel = fl::DamageLevel::Intact;
    st.dead = false;
    auto ctrl = c->sample(st, 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: state player_owned and owner_id accessible") {
    // player_owned=false, owner_id=3 → throttle=1 when owner_id==3
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  if not s.player_owned and s.owner_id==3 then return {throttle=1.0}\n"
                      "  else return {} end\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: state type_index accessible") {
    // type_index=7 → throttle=1 when type_index==7
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  if s.type_index==7 then return {throttle=1.0} else return {} end\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: runtime error in compute_control returns neutral ControlInput") {
    auto c = makeCtrl("function compute_control(s,t,dt) error('deliberate') end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.f));
    CHECK(ctrl.elevator == Catch::Approx(0.f));
    CHECK_FALSE(ctrl.afterburner);
}

TEST_CASE("LuaController: missing compute_control function returns neutral ControlInput") {
    auto c = makeCtrl("-- no function defined");
    REQUIRE(c->isValid()); // script loads fine; function just missing
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.f));
}

TEST_CASE("LuaController: syntax error sets isValid false and lastError non-empty") {
    auto c = makeCtrl("this is not valid lua @@@@");
    CHECK_FALSE(c->isValid());
    CHECK_FALSE(c->lastError().empty());
}

// ---------------------------------------------------------------------------
// Guidance module
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: guidance heading_error callable and returns number") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local q = s.quat\n"
                      "  local err = guidance.heading_error(q, s.pos, {x=3000,y=600,z=0})\n"
                      "  return {throttle = (type(err)=='number') and 1.0 or 0.0}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: guidance pitch_error_from_alt callable and returns number") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local err = guidance.pitch_error_from_alt(s.quat, 100.0)\n"
                      "  return {throttle = (type(err)=='number') and 1.0 or 0.0}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: guidance bank_to_turn_aileron callable and in range") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local a = guidance.bank_to_turn_aileron(0.5)\n"
                      "  return {aileron=a}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.aileron >= -1.f);
    CHECK(ctrl.aileron <= 1.f);
    CHECK(ctrl.aileron != Catch::Approx(0.f).epsilon(0.001f));
}

TEST_CASE("LuaController: guidance coordinated_rudder callable and in range") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local r = guidance.coordinated_rudder(0.6)\n"
                      "  return {rudder=r}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.rudder >= -1.f);
    CHECK(ctrl.rudder <= 1.f);
}

TEST_CASE("LuaController: guidance elevator_from_pitch_error callable and in range") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local e = guidance.elevator_from_pitch_error(0.3)\n"
                      "  return {elevator=e}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.elevator >= -1.f);
    CHECK(ctrl.elevator <= 1.f);
    CHECK(ctrl.elevator != Catch::Approx(0.f).epsilon(0.001f));
}

TEST_CASE("LuaController: guidance body_forward returns table with x y z fields") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local f = guidance.body_forward(s.quat)\n"
                      "  local ok = type(f.x)=='number' and type(f.y)=='number' and type(f.z)=='number'\n"
                      "  return {throttle = ok and 1.0 or 0.0}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

// ---------------------------------------------------------------------------
// Spatial / entity queries
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: nearby_entities with null si returns empty table") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local nb = nearby_entities(0,0,5000)\n"
                      "  return {throttle = (#nb == 0) and 1.0 or 0.0}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0, nullptr);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: nearby_entities with real SpatialIndex returns idx and pos") {
    fl::SpatialIndex si;
    double pos[3] = {100.0, 600.0, 200.0};
    si.insert(42, pos);

    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local nb = nearby_entities(0,0,1000)\n"
                      "  if #nb == 1 and nb[1].idx == 42 then\n"
                      "    return {throttle=1.0}\n"
                      "  end\n"
                      "  return {}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0, &si);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: get_entity with null entityManager returns nil") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local e = get_entity(1)\n"
                      "  return {throttle = (e==nil) and 1.0 or 0.0}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: get_entity with real EntityManager returns state table") {
    NullLoggerL log;
    fl::EntityTypeRegistry reg;
    fl::EntityDef def;
    def.id = "test:plane";
    def.name = "Plane";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 80.f;
    reg.registerType(std::move(def));

    fl::EntityManager em(log, reg);
    fl::EntityTransform t{};
    t.pos[0] = 0.0;
    t.pos[1] = 600.0;
    t.pos[2] = 0.0;
    fl::EntityId id = em.spawn("test:plane", t);
    REQUIRE(id.valid());

    // Script retrieves entity by index and checks hp via max_hp (80).
    auto src = std::string("function compute_control(s,t,dt)"
                           "  local e = get_entity(") +
               std::to_string(id.index) +
               std::string(")"
                           "  if e ~= nil and e.max_hp == 80.0 then return {throttle=1.0} end"
                           "  return {}"
                           "end");

    LuaController c(src, "", &em);
    REQUIRE(c.isValid());
    auto ctrl = c.sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

// ---------------------------------------------------------------------------
// Lifecycle / persistence
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: Lua state persists between sample calls") {
    // Module-level counter incremented each tick.
    auto c = makeCtrl("local n = 0"
                      "\nfunction compute_control(s,t,dt)"
                      "  n = n + 1"
                      "  return {throttle = n * 1.0}"
                      "end");
    REQUIRE(c->isValid());
    auto st = makeState();
    auto ctrl1 = c->sample(st, 0, 1.0 / 60.0);
    auto ctrl2 = c->sample(st, 1, 1.0 / 60.0);
    CHECK(ctrl1.throttle == Catch::Approx(1.f).epsilon(0.001f));
    CHECK(ctrl2.throttle == Catch::Approx(2.f).epsilon(0.001f));
}

TEST_CASE("LuaController: script uses require from pack ai dir") {
    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path() / "fl_lua_ctrl_test";
    auto aiDir = tmpDir / "ai";
    fs::create_directories(aiDir);

    {
        std::ofstream f(aiDir / "util.lua");
        f << "return { add = function(a,b) return a+b end }\n";
    }

    const char* src = "local util = require('util')\n"
                      "function compute_control(s,t,dt)\n"
                      "  return {throttle = util.add(0.4, 0.35)}\n"
                      "end\n";

    LuaController c(src, tmpDir.string());
    REQUIRE(c.isValid());
    auto ctrl = c.sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.75f).epsilon(0.001f));

    fs::remove_all(tmpDir);
}
