// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "atc/AtcService.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "script/BuiltinAiScripts.h"
#include "script/LuaController.h"
#include "script/WorldApi.h"
#include "sensor/SensorSystem.h"
#include "spatial/SpatialIndex.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"

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
                      "  local err = guidance.pitch_error_from_alt(s.quat, s.pos, 100.0)\n"
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
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0, fl::AiTickContext{});
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
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0, fl::AiTickContext{&si});
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

// --- detected_contacts() (#691) ------------------------------------------------------------------

namespace {

// A contact table a script can be handed directly. The sensing pass builds these for real; here we
// inject one so the BINDING is under test, not the detection math (test_sensor_system covers that).
fl::sensor::ContactTable makeContacts() {
    fl::sensor::Contact c{};
    c.id.index = 42;
    c.id.generation = 1;
    c.factionIndex = 2;
    c.state = fl::sensor::ContactState::Locked;
    c.lastKnownPos[0] = 1000.0;
    c.lastKnownPos[1] = 500.0;
    c.lastKnownPos[2] = -250.0;
    c.lastKnownVel[0] = 200.f;
    c.lastSeenTick = 100;
    c.firstDetectedTick = 50;
    c.reacted = true;
    c.sensorTypeMask = static_cast<uint8_t>(1u << static_cast<int>(fl::sensor::SensorType::Radar)) |
                       static_cast<uint8_t>(1u << static_cast<int>(fl::sensor::SensorType::Visual));

    fl::sensor::ContactTable t;
    t.contacts.push_back(c);
    return t;
}

} // namespace

TEST_CASE("LuaController: detected_contacts returns the honest view of what the entity has found") {
    const fl::sensor::ContactTable contacts = makeContacts();

    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local cs = detected_contacts()\n"
                      "  if #cs ~= 1 then return {} end\n"
                      "  local k = cs[1]\n"
                      "  if k.idx == 42 and k.state == 'locked' and k.reacted == true\n"
                      "     and k.faction == 2 and k.pos.x == 1000.0 and k.vel.x == 200.0 then\n"
                      "    return {throttle=1.0}\n"
                      "  end\n"
                      "  return {}\n"
                      "end");
    REQUIRE(c->isValid());

    fl::AiTickContext ctx{};
    ctx.contacts = &contacts;
    const auto ctrl = c->sample(makeState(), 100, 1.0 / 60.0, ctx);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: detected_contacts reports which SENSOR TYPES hold the contact") {
    // "He has me on radar" and "he can see me" are different tactical facts, and a script is
    // entitled to tell them apart.
    const fl::sensor::ContactTable contacts = makeContacts();

    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local k = detected_contacts()[1]\n"
                      "  local has_radar, has_visual, has_ir = false, false, false\n"
                      "  for _, ty in ipairs(k.sensor_types) do\n"
                      "    if ty == 'radar'  then has_radar  = true end\n"
                      "    if ty == 'visual' then has_visual = true end\n"
                      "    if ty == 'ir'     then has_ir     = true end\n"
                      "  end\n"
                      "  if has_radar and has_visual and not has_ir then return {throttle=1.0} end\n"
                      "  return {}\n"
                      "end");
    REQUIRE(c->isValid());

    fl::AiTickContext ctx{};
    ctx.contacts = &contacts;
    const auto ctrl = c->sample(makeState(), 100, 1.0 / 60.0, ctx);
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: a stale contact reports its AGE, so a script knows not to trust pos") {
    // 30 ticks after it was last actually seen, at 60 Hz, age_s = 0.5.
    const fl::sensor::ContactTable contacts = makeContacts();

    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local k = detected_contacts()[1]\n"
                      "  if math.abs(k.age_s - 0.5) < 0.001 then return {throttle=1.0} end\n"
                      "  return {}\n"
                      "end");
    REQUIRE(c->isValid());

    fl::AiTickContext ctx{};
    ctx.contacts = &contacts;
    const auto ctrl = c->sample(makeState(), 130, 1.0 / 60.0, ctx); // lastSeenTick = 100
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

TEST_CASE("LuaController: detected_contacts returns an empty table when sensing was not evaluated") {
    // Null contacts = "sensing did not run here" (headless callers, tests). Existing scripts must
    // keep working unchanged, so this is an empty table rather than an error.
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  local cs = detected_contacts()\n"
                      "  return {throttle = (#cs == 0) and 1.0 or 0.0}\n"
                      "end");
    REQUIRE(c->isValid());

    const auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0, fl::AiTickContext{});
    CHECK(ctrl.throttle == Catch::Approx(1.f).epsilon(0.001f));
}

// --- the documented example actually runs (#694) --------------------------------------------------

TEST_CASE("LuaController: the detected_contacts() example from docs/modding/ai.md runs as documented") {
    // The acceptance bullet for #694 is "docs/modding/ai.md examples run against the implemented Lua
    // API". A doc example that has never been executed is a promise, not a fact — so this IS the
    // example, verbatim from the guide. If someone changes the API and forgets the docs, this fails.
    auto c = makeCtrl("function compute_control(state, tick, dt)\n"
                      "    for _, c in ipairs(detected_contacts()) do\n"
                      "        if c.reacted and c.faction ~= 0 and c.faction ~= state.faction then\n"
                      "            local herr = guidance.heading_error(state.quat, state.pos, c.pos)\n"
                      "            return { aileron = guidance.bank_to_turn_aileron(herr), throttle = 1.0 }\n"
                      "        end\n"
                      "    end\n"
                      "    return { throttle = 0.6 }   -- nothing detected: no target to chase\n"
                      "end");
    REQUIRE(c->isValid());

    // With no contacts it cruises — it does not chase a target it has not found.
    const fl::ControlInput idle = c->sample(makeState(), 0, 1.0 / 60.0, fl::AiTickContext{});
    CHECK(idle.throttle == Catch::Approx(0.6f).epsilon(0.001f));

    // With a reacted hostile contact off to the right (+Z), it firewalls the throttle and banks
    // toward the contact's LAST-KNOWN position.
    fl::sensor::Contact k{};
    k.id.index = 7;
    k.id.generation = 1;
    k.state = fl::sensor::ContactState::Locked;
    k.reacted = true;
    k.factionIndex = 2;
    k.lastKnownPos[2] = 5000.0; // to the right of a nose-along-+X aircraft

    fl::sensor::ContactTable table;
    table.contacts.push_back(k);

    fl::AiTickContext ctx{};
    ctx.contacts = &table;

    fl::EntityState self = makeState();
    self.factionIndex = 1; // hostile to faction 2

    const fl::ControlInput engaged = c->sample(self, 0, 1.0 / 60.0, ctx);
    CHECK(engaged.throttle == Catch::Approx(1.0f).epsilon(0.001f));
    CHECK(engaged.aileron > 0.f); // banking right, toward the contact
}

TEST_CASE("LuaController: the Script-anatomy loiter example from docs/modding/ai.md runs as documented") {
    // #830: the doc's own worked example called pitch_error_from_alt with the two-argument form the
    // guide mis-documented, so anyone who copied it got a Lua error every tick and an AI that flew
    // straight ahead forever. This is that example, verbatim from the guide (only the loiter centre
    // constants matter to the assertions). If the binding signature and the doc drift again, this
    // fails instead of the next content author's evening.
    auto c = makeCtrl("local cx, cz, alt = 0, 0, 600\n"
                      "local radius = 3000\n"
                      "\n"
                      "function compute_control(state, tick, dt)\n"
                      "    local pos  = state.pos\n"
                      "    local quat = state.quat\n"
                      "    local nx   = cx - pos.x\n"
                      "    local nz   = cz - pos.z\n"
                      "    local dist = math.sqrt(nx * nx + nz * nz)\n"
                      "    if dist < 1 then\n"
                      "        return {throttle = 0.65}\n"
                      "    end\n"
                      "    nx, nz = nx / dist, nz / dist\n"
                      "    local tx   = pos.x + nx * math.min(dist, 1000) + nz * 1000\n"
                      "    local tz   = pos.z + nz * math.min(dist, 1000) - nx * 1000\n"
                      "    local herr = guidance.heading_error(quat, pos, {x = tx, y = pos.y, z = tz})\n"
                      "    local perr = guidance.pitch_error_from_alt(quat, pos, alt - pos.y)\n"
                      "    return {\n"
                      "        aileron  = guidance.bank_to_turn_aileron(herr),\n"
                      "        rudder   = guidance.coordinated_rudder(guidance.bank_to_turn_aileron(herr)),\n"
                      "        elevator = guidance.elevator_from_pitch_error(perr),\n"
                      "        throttle = 0.65,\n"
                      "    }\n"
                      "end");
    REQUIRE(c->isValid());

    // Away from the loiter centre the full guidance path runs. A Lua error in any of the calls
    // would surface as the neutral ControlInput (throttle 0) — asserting 0.65 IS the regression
    // check for the mis-documented two-argument pitch_error_from_alt.
    const fl::ControlInput steering = c->sample(makeState(5000.0, 600.0, 0.0), 0, 1.0 / 60.0, fl::AiTickContext{});
    CHECK(steering.throttle == Catch::Approx(0.65f).epsilon(0.001f));
    CHECK(steering.aileron != 0.f); // it is actually steering, not coasting through an error

    // At the centre the early-out branch runs.
    const fl::ControlInput centred = c->sample(makeState(0.0, 600.0, 0.0), 0, 1.0 / 60.0, fl::AiTickContext{});
    CHECK(centred.throttle == Catch::Approx(0.65f).epsilon(0.001f));
    CHECK(centred.aileron == 0.f);
}

TEST_CASE("LuaController: state.faction lets a script tell friend from foe") {
    // The gap the documented example exposed: `detected_contacts()` reports each contact's faction,
    // but a script had no way to learn its OWN — so `c.faction ~= state.faction` compared against nil,
    // every contact looked hostile, and the documented AI would have opened fire on its own wingman.
    auto c = makeCtrl("function compute_control(state, tick, dt)\n"
                      "    for _, k in ipairs(detected_contacts()) do\n"
                      "        if k.reacted and k.faction ~= 0 and k.faction ~= state.faction then\n"
                      "            return { throttle = 1.0 }   -- engage\n"
                      "        end\n"
                      "    end\n"
                      "    return { throttle = 0.6 }           -- hold\n"
                      "end");
    REQUIRE(c->isValid());

    fl::sensor::Contact k{};
    k.id.index = 7;
    k.id.generation = 1;
    k.state = fl::sensor::ContactState::Locked;
    k.reacted = true;
    k.factionIndex = 1; // SAME faction as self — a friendly

    fl::sensor::ContactTable table;
    table.contacts.push_back(k);
    fl::AiTickContext ctx{};
    ctx.contacts = &table;

    fl::EntityState self = makeState();
    self.factionIndex = 1;

    const fl::ControlInput ctrl = c->sample(self, 0, 1.0 / 60.0, ctx);
    CHECK(ctrl.throttle == Catch::Approx(0.6f).epsilon(0.001f)); // holds fire on its own side
}

// ---------------------------------------------------------------------------
// Fire intent (#625)
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: trigger, release, and weapon_station reach ControlInput") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {trigger=true, release=true, weapon_station=2} end");
    REQUIRE(c->isValid());
    const fl::ControlInput ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.trigger);
    CHECK(ctrl.release);
    CHECK(ctrl.station == 2u);
}

TEST_CASE("LuaController: absent fire fields default to no intent and keep the station") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {throttle=0.5} end");
    REQUIRE(c->isValid());
    const fl::ControlInput ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK_FALSE(ctrl.trigger);
    CHECK_FALSE(ctrl.release);
    CHECK(ctrl.station == 255u); // "keep" — the wire sentinel, same as an untouched player selector
}

TEST_CASE("LuaController: an out-of-range weapon_station is ignored, not wrapped") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {weapon_station=999} end");
    REQUIRE(c->isValid());
    const fl::ControlInput ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.station == 255u); // a nonsense selection must not become a real one
}

// --- builtin AI scripts (#866) -------------------------------------------------------------------

TEST_CASE("builtinAiScript resolves builtin:fighter and rejects the unknown (#866)") {
    CHECK_FALSE(fl::builtinAiScript("builtin:fighter").empty());
    CHECK(fl::builtinAiScript("builtin:nope").empty());
    // The id list is what a frontend seeds its AI-script cache from.
    bool sawFighter = false;
    for (std::string_view id : fl::builtinAiScriptIds())
        if (id == "builtin:fighter")
            sawFighter = true;
    CHECK(sawFighter);
}

TEST_CASE("the builtin fighter script compiles and drives an entity (#866)") {
    auto c = makeCtrl(std::string(fl::builtinAiScript("builtin:fighter")).c_str());
    REQUIRE(c->isValid());
    // No contacts: it patrols (a valid, non-crashing control), exercising the sandbox path zero-pack.
    const auto ctrl = c->sample(makeState(0.0, 3000.0, 0.0), 100, 1.0 / 60.0, fl::AiTickContext{});
    CHECK(ctrl.throttle > 0.f); // flying its orbit
}

TEST_CASE("the builtin fighter senses via detected_contacts and fires the gun in parameters (#866)") {
    // A hostile (faction 2 vs the ownship's 0) 700 m dead ahead on the nose, reacted and fresh:
    // inside gun range and cone, so the honest-sensing script takes the guns snapshot.
    fl::sensor::Contact bandit{};
    bandit.id = {42, 1};
    bandit.factionIndex = 2;
    bandit.state = fl::sensor::ContactState::Locked;
    bandit.lastKnownPos[0] = 700.0;
    bandit.lastKnownPos[1] = 3000.0;
    bandit.lastKnownPos[2] = 0.0;
    bandit.lastSeenTick = 100;
    bandit.firstDetectedTick = 40;
    bandit.reacted = true;
    fl::sensor::ContactTable contacts;
    contacts.contacts.push_back(bandit);

    auto c = makeCtrl(std::string(fl::builtinAiScript("builtin:fighter")).c_str());
    REQUIRE(c->isValid());

    fl::AiTickContext ctx{};
    ctx.contacts = &contacts;
    const auto ctrl = c->sample(makeState(0.0, 3000.0, 0.0), 100, 1.0 / 60.0, ctx);
    CHECK(ctrl.trigger);       // guns hot on a boresight target in range
    CHECK(ctrl.station == 0u); // the cannon station (slot 0 of builtin:debug-entity)
}

// ---------------------------------------------------------------------------
// Coroutine control flow (#412)
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: an ai_main coroutine yields a control table each tick (#412)") {
    // A sequential state machine: full throttle on the first two ticks, then afterburner forever.
    auto c = makeCtrl("function ai_main()\n"
                      "  coroutine.yield({throttle=1.0})\n"
                      "  coroutine.yield({throttle=1.0})\n"
                      "  while true do\n"
                      "    coroutine.yield({throttle=1.0, afterburner=true})\n"
                      "  end\n"
                      "end\n");
    REQUIRE(c->isValid());

    auto t0 = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(t0.throttle == Catch::Approx(1.0f));
    CHECK_FALSE(t0.afterburner);

    auto t1 = c->sample(makeState(), 1, 1.0 / 60.0);
    CHECK(t1.throttle == Catch::Approx(1.0f));
    CHECK_FALSE(t1.afterburner);

    auto t2 = c->sample(makeState(), 2, 1.0 / 60.0);
    CHECK(t2.throttle == Catch::Approx(1.0f));
    CHECK(t2.afterburner); // reached the loop

    auto t3 = c->sample(makeState(), 3, 1.0 / 60.0);
    CHECK(t3.afterburner); // stays in the loop
}

TEST_CASE("LuaController: ai_main sees the resumed state and can branch on it (#412)") {
    // yield's return value is the next (state, tick, dt): the script reads hp and evades when hurt.
    auto c = makeCtrl("function ai_main()\n"
                      "  local state = coroutine.yield({throttle=0.5})\n"
                      "  while true do\n"
                      "    if state.hp < state.max_hp * 0.5 then\n"
                      "      state = coroutine.yield({throttle=1.0, afterburner=true})\n"
                      "    else\n"
                      "      state = coroutine.yield({throttle=0.5})\n"
                      "    end\n"
                      "  end\n"
                      "end\n");
    REQUIRE(c->isValid());

    c->sample(makeState(0, 600, 0, 100.f, 100.f), 0, 1.0 / 60.0); // primes ai_main
    auto healthy = c->sample(makeState(0, 600, 0, 100.f, 100.f), 1, 1.0 / 60.0);
    CHECK_FALSE(healthy.afterburner);
    auto hurt = c->sample(makeState(0, 600, 0, 20.f, 100.f), 2, 1.0 / 60.0);
    CHECK(hurt.afterburner); // hp below half -> evade branch
}

TEST_CASE("LuaController: a finished ai_main coroutine goes neutral forever (#412)") {
    auto c = makeCtrl("function ai_main()\n"
                      "  coroutine.yield({throttle=1.0})\n"
                      "  -- returns here: behavior finished\n"
                      "end\n");
    REQUIRE(c->isValid());

    auto t0 = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(t0.throttle == Catch::Approx(1.0f));
    auto t1 = c->sample(makeState(), 1, 1.0 / 60.0); // ai_main returns -> neutral
    CHECK(t1.throttle == Catch::Approx(0.f));
    auto t2 = c->sample(makeState(), 2, 1.0 / 60.0); // stays neutral
    CHECK(t2.throttle == Catch::Approx(0.f));
}

TEST_CASE("LuaController: a coroutine yielding nothing produces neutral control (#412)") {
    auto c = makeCtrl("function ai_main()\n"
                      "  while true do coroutine.yield() end\n"
                      "end\n");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.f));
    CHECK_FALSE(ctrl.afterburner);
}

TEST_CASE("LuaController: an ai_main coroutine can call guidance and detected_contacts (#412)") {
    // The coroutine shares globals with the sandbox, so the honest-sensing bindings work inside it.
    auto c = makeCtrl("function ai_main()\n"
                      "  while true do\n"
                      "    local n = #detected_contacts()\n"
                      "    coroutine.yield({throttle = (n > 0) and 1.0 or 0.2})\n"
                      "  end\n"
                      "end\n");
    REQUIRE(c->isValid());

    fl::sensor::Contact bandit{};
    bandit.id = {42, 1};
    bandit.factionIndex = 2;
    bandit.state = fl::sensor::ContactState::Locked;
    bandit.lastKnownPos[0] = 700.0;
    bandit.lastSeenTick = 10;
    bandit.reacted = true;
    fl::sensor::ContactTable contacts;
    contacts.contacts.push_back(bandit);

    auto empty = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(empty.throttle == Catch::Approx(0.2f)); // no contacts (null table)

    fl::AiTickContext ctx{};
    ctx.contacts = &contacts;
    auto seen = c->sample(makeState(), 1, 1.0 / 60.0, ctx);
    CHECK(seen.throttle == Catch::Approx(1.0f)); // one detected contact
}

TEST_CASE("LuaController: ai_main takes precedence when both entry points are defined (#412)") {
    auto c = makeCtrl("function compute_control(s,t,dt) return {throttle=0.1} end\n"
                      "function ai_main()\n"
                      "  while true do coroutine.yield({throttle=0.9}) end\n"
                      "end\n");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.9f)); // the coroutine, not compute_control
}

// ---------------------------------------------------------------------------
// world.* module (#413)
// ---------------------------------------------------------------------------

namespace {
// A WorldApi that records every call, for asserting the bindings reach the host seam.
struct RecordingWorld {
    fl::WorldApi api;
    std::vector<std::string> spawns; // "type|x,y,z|heading|side"
    std::vector<int> despawns;
    std::vector<std::string> relations; // "a|b|rel"
    std::vector<std::string> music;
    std::vector<bool> outcomes;
    std::vector<std::pair<int, int>> objectives; // (faction, count)
    std::vector<std::string> alerts;             // "faction|level" (#162)
    int nextIdx{100};

    RecordingWorld() {
        api.spawn = [this](const std::string& t, const std::array<double, 3>& p, float h, const std::string& side) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s|%.0f,%.0f,%.0f|%.0f|%s", t.c_str(), p[0], p[1], p[2],
                          static_cast<double>(h), side.c_str());
            spawns.emplace_back(buf);
            return nextIdx++;
        };
        api.despawn = [this](int idx) { despawns.push_back(idx); };
        api.setRelationship = [this](const std::string& a, const std::string& b, const std::string& r) {
            relations.push_back(a + "|" + b + "|" + r);
        };
        api.setMusicState = [this](const std::string& s) { music.push_back(s); };
        api.setAlertLevel = [this](const std::string& f, const std::string& lvl) { alerts.push_back(f + "|" + lvl); };
        api.getAlertLevel = [](const std::string& f) {
            return f == "russia" ? std::string("conflict") : std::string("peacetime");
        };
        api.getZoneStage = [](int idx, const std::string& zone) {
            return (idx == 7 && zone == "capital") ? std::string("warned") : std::string("clean");
        };
        api.isInZone = [](int idx, const std::string& zone) { return idx == 7 && zone == "capital"; };
        api.setMissionOutcome = [this](bool ok) { outcomes.push_back(ok); };
        api.scoreObjective = [this](int faction, int count) { objectives.emplace_back(faction, count); };
    }
};

std::unique_ptr<LuaController> makeWorldCtrl(const char* src, const fl::WorldApi* api) {
    return std::make_unique<LuaController>(src, "", nullptr, api);
}
} // namespace

TEST_CASE("world.spawn routes to the host and returns the new entity index (#413)") {
    RecordingWorld w;
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  if t == 0 then\n"
                           "    spawned = world.spawn('Su27', {x=100, y=0, z=200}, 90, 'russia')\n"
                           "  end\n"
                           "  return {throttle = (spawned == 100) and 1.0 or 0.0}\n"
                           "end",
                           &w.api);
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    REQUIRE(w.spawns.size() == 1u);
    CHECK(w.spawns[0] == "Su27|100,0,200|90|russia");
    CHECK(ctrl.throttle == Catch::Approx(1.0f)); // saw the returned idx 100
}

TEST_CASE("world.spawn defaults side to neutral and returns -1 with no host (#413)") {
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  return {throttle = world.spawn('X', {x=0,y=0,z=0}, 0) < 0 and 0.5 or 0.0}\n"
                           "end",
                           nullptr); // no WorldApi
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.5f)); // -1 => no host
}

TEST_CASE("world.score_objective routes faction + count to the host (#1000)") {
    RecordingWorld w;
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  if t == 0 then\n"
                           "    world.score_objective(2)\n"    // default count 1
                           "    world.score_objective(1, 3)\n" // explicit count
                           "  end\n"
                           "  return {}\n"
                           "end",
                           &w.api);
    REQUIRE(c->isValid());
    c->sample(makeState(), 0, 1.0 / 60.0);
    REQUIRE(w.objectives.size() == 2u);
    CHECK(w.objectives[0] == std::pair<int, int>{2, 1});
    CHECK(w.objectives[1] == std::pair<int, int>{1, 3});
}

TEST_CASE("world.score_objective is a safe no-op with no host (#1000)") {
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  world.score_objective(1, 2)\n"
                           "  return {}\n"
                           "end",
                           nullptr);
    REQUIRE(c->isValid());
    c->sample(makeState(), 0, 1.0 / 60.0); // must not crash
    SUCCEED();
}

TEST_CASE("world.despawn / set_relationship / set_music_state / mission outcome reach the host (#413)") {
    RecordingWorld w;
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  world.despawn(42)\n"
                           "  world.set_relationship('nato', 'russia', 'hostile')\n"
                           "  world.set_music_state('combat')\n"
                           "  world.mission_success()\n"
                           "  return {}\n"
                           "end",
                           &w.api);
    REQUIRE(c->isValid());
    c->sample(makeState(), 0, 1.0 / 60.0);
    REQUIRE(w.despawns.size() == 1u);
    CHECK(w.despawns[0] == 42);
    REQUIRE(w.relations.size() == 1u);
    CHECK(w.relations[0] == "nato|russia|hostile");
    REQUIRE(w.music.size() == 1u);
    CHECK(w.music[0] == "combat");
    REQUIRE(w.outcomes.size() == 1u);
    CHECK(w.outcomes[0] == true);
}

TEST_CASE("world.timer fires its callback once after N sim-seconds (#413)") {
    RecordingWorld w;
    auto c = makeWorldCtrl("fired = 0\n"
                           "function compute_control(s,t,dt)\n"
                           "  if t == 0 then world.timer(1.0, function() fired = fired + 1 end) end\n"
                           "  return {throttle = fired}\n"
                           "end",
                           &w.api);
    REQUIRE(c->isValid());
    // 60 ticks at 1/60 s = 1.0 s. Timer should fire exactly once at/after the deadline.
    float last = 0.f;
    for (uint64_t t = 0; t < 120; ++t)
        last = c->sample(makeState(), t, 1.0 / 60.0).throttle;
    CHECK(last == Catch::Approx(1.0f)); // fired exactly once, never re-fires
}

TEST_CASE("world.on_trigger fires once when its predicate first returns true (#413)") {
    RecordingWorld w;
    // Predicate becomes true at elapsed >= 0.5 s (30 ticks). Callback ends the mission.
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  if t == 0 then\n"
                           "    world.on_trigger(function() return world.get_elapsed_time() >= 0.5 end,\n"
                           "                     function() world.mission_failure() end)\n"
                           "  end\n"
                           "  return {}\n"
                           "end",
                           &w.api);
    REQUIRE(c->isValid());
    for (uint64_t t = 0; t < 20; ++t)
        c->sample(makeState(), t, 1.0 / 60.0);
    CHECK(w.outcomes.empty()); // not yet — under 0.5 s
    for (uint64_t t = 20; t < 60; ++t)
        c->sample(makeState(), t, 1.0 / 60.0);
    REQUIRE(w.outcomes.size() == 1u); // fired exactly once
    CHECK(w.outcomes[0] == false);
}

TEST_CASE("world.* bindings are present even without a host and world.get_elapsed_time advances (#413)") {
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  return {throttle = world.get_elapsed_time()}\n"
                           "end",
                           nullptr);
    REQUIRE(c->isValid());
    c->sample(makeState(), 0, 1.0 / 60.0);
    auto ctrl = c->sample(makeState(), 1, 0.5); // +0.5 s
    CHECK(ctrl.throttle > 0.5f);                // elapsed accumulated across ticks
}

// ---------------------------------------------------------------------------
// Haptics (#128)
// ---------------------------------------------------------------------------

namespace {
struct RecordingHaptics {
    fl::WorldApi api;
    std::vector<std::string> rumbles;  // "low,high,dur"
    std::vector<std::string> triggers; // "left,right,dur"
    int stops{0};
    RecordingHaptics() {
        api.rumble = [this](float low, float high, uint32_t dur) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.2f,%.2f,%u", low, high, dur);
            rumbles.emplace_back(b);
        };
        api.rumbleTriggers = [this](float l, float r, uint32_t dur) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.2f,%.2f,%u", l, r, dur);
            triggers.emplace_back(b);
        };
        api.stopRumble = [this]() { ++stops; };
    }
};
} // namespace

TEST_CASE("rumble / rumble_triggers / stop_rumble route to the host seam (#128)") {
    RecordingHaptics h;
    auto c = std::make_unique<LuaController>("function compute_control(s,t,dt)\n"
                                             "  rumble(0.5, 0.8, 200)\n"
                                             "  rumble_triggers(0.1, 0.2, 100)\n"
                                             "  stop_rumble()\n"
                                             "  return {}\n"
                                             "end",
                                             "", nullptr, &h.api);
    REQUIRE(c->isValid());
    c->sample(makeState(), 0, 1.0 / 60.0);
    REQUIRE(h.rumbles.size() == 1u);
    CHECK(h.rumbles[0] == "0.50,0.80,200");
    REQUIRE(h.triggers.size() == 1u);
    CHECK(h.triggers[0] == "0.10,0.20,100");
    CHECK(h.stops == 1);
}

TEST_CASE("rumble clamps intensity to [0,1] and duration to the cap (#128)") {
    RecordingHaptics h;
    // A mod cannot lock rumble on: intensities clamp to 1.0, duration to 5000 ms.
    auto c = std::make_unique<LuaController>("function compute_control(s,t,dt)\n"
                                             "  rumble(9.0, -3.0, 999999)\n"
                                             "  return {}\n"
                                             "end",
                                             "", nullptr, &h.api);
    REQUIRE(c->isValid());
    c->sample(makeState(), 0, 1.0 / 60.0);
    REQUIRE(h.rumbles.size() == 1u);
    CHECK(h.rumbles[0] == "1.00,0.00,5000"); // clamped
}

TEST_CASE("haptic bindings are present and safe no-ops without a host (#128)") {
    auto c = makeCtrl("function compute_control(s,t,dt)\n"
                      "  rumble(1,1,100); stop_rumble()\n"
                      "  return {throttle=0.3}\n"
                      "end");
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0); // no WorldApi -> no crash
    CHECK(ctrl.throttle == Catch::Approx(0.3f));
}

// ---------------------------------------------------------------------------
// atc.* module (#705)
// ---------------------------------------------------------------------------

TEST_CASE("LuaController: atc.scramble triggers the spawn handler (#705)", "[lua][atc]") {
    // #673 criterion 3: a Lua AI script launches a flight from a named airport.
    NullLoggerL log;
    fl::EntityTypeRegistry reg;
    fl::EntityDef d;
    d.id = "test:basic";
    d.name = "B";
    d.category = fl::ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    reg.registerType(d);
    fl::EntityManager em(log, reg);

    fl::AirportRegistry airports;
    airports.load({fl::builtinAirfield()}, fl::kEarthRadiusM, nullptr);
    fl::atc::AtcService atc(em, airports, fl::kEarthRadiusM);
    int spawns = 0;
    atc.setSpawnHandler([&](const fl::atc::AtcService::DepartureSpawn& s) {
        ++spawns;
        CHECK(s.facilityId == "builtin:airfield");
        CHECK(s.typeId == "test:basic");
    });

    const char* src = "function compute_control(state, tick, dt)\n"
                      "  if tick == 0 then atc.scramble('builtin:airfield', 'test:basic', 2) end\n"
                      "  return {}\n"
                      "end";
    fl::LuaController ctrl(src, "", &em, nullptr, &atc);
    REQUIRE(ctrl.isValid());
    ctrl.sample(makeState(), 0, 1.0 / 60.0);
    CHECK(spawns == 2);
}

TEST_CASE("LuaController: atc.* is nil-safe with no ATC service (#705)", "[lua][atc]") {
    // request_takeoff() returns false and clearance() returns "none" when no service is wired; the
    // script maps those to control fields so we can observe them through the ControlInput return.
    const char* src = "function compute_control(state, tick, dt)\n"
                      "  local ok = atc.request_takeoff()\n"
                      "  local none = (atc.clearance() == 'none')\n"
                      "  return { throttle = (ok and 1.0 or 0.25), aileron = (none and 0.5 or 0.0) }\n"
                      "end";
    auto c = makeCtrl(src); // no atc service
    REQUIRE(c->isValid());
    auto ctrl = c->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(ctrl.throttle == Catch::Approx(0.25f)); // request refused
    CHECK(ctrl.aileron == Catch::Approx(0.5f));   // clearance == "none"
}

TEST_CASE("LuaController: atc.request_takeoff sequences the entity when a service is wired (#705)", "[lua][atc]") {
    NullLoggerL log;
    fl::EntityTypeRegistry reg;
    fl::EntityDef d;
    d.id = "test:basic";
    d.name = "B";
    d.category = fl::ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    reg.registerType(d);
    fl::EntityManager em(log, reg);
    fl::AirportRegistry airports;
    airports.load({fl::builtinAirfield()}, fl::kEarthRadiusM, nullptr);
    fl::atc::AtcService atc(em, airports, fl::kEarthRadiusM);

    // Spawn the controlled entity near the field so nearest-airport resolution finds it.
    const fl::ResolvedAirport* field = airports.byId("builtin:airfield");
    REQUIRE(field != nullptr);
    fl::EntityTransform t{};
    t.pos[0] = field->worldPos.x;
    t.pos[2] = field->worldPos.z;
    t.quat[3] = 1.f;
    fl::EntityId id = em.spawn("test:basic", t);

    const char* src = "function compute_control(state, tick, dt)\n"
                      "  if tick == 0 then atc.request_takeoff() end\n"
                      "  return { throttle = (atc.clearance() == 'hold_short') and 1.0 or 0.0 }\n"
                      "end";
    fl::LuaController ctrl(src, "", &em, nullptr, &atc);
    REQUIRE(ctrl.isValid());
    fl::EntityState* s = em.get(id);
    REQUIRE(s != nullptr);
    auto out = ctrl.sample(*s, 0, 1.0 / 60.0);
    // After request_takeoff on tick 0 the entity is holding short, which the script reports as throttle 1.
    CHECK(out.throttle == Catch::Approx(1.0f));
    CHECK(atc.clearanceState(id) == fl::atc::ClearanceState::HoldShort);
}

TEST_CASE("world alert-level and zone bindings route to the host (#162)") {
    RecordingWorld w;
    // The query results are reported back through set_relationship, which RecordingWorld already
    // captures -- so the test reads what the script actually saw without reaching into the sandbox.
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  if t == 0 then\n"
                           "    world.set_alert_level('russia', 'war_state')\n"
                           "    world.set_relationship(world.get_alert_level('russia'),\n"
                           "                           world.get_zone_stage(7, 'capital'),\n"
                           "                           tostring(world.is_in_zone(7, 'capital')))\n"
                           "  end\n"
                           "  return { throttle = 0.5 }\n"
                           "end\n",
                           &w.api);
    REQUIRE(c->isValid());
    (void)c->sample(EntityState{}, 0, 1.0 / 60.0);

    REQUIRE(w.alerts.size() == 1);
    CHECK(w.alerts[0] == "russia|war_state");
    REQUIRE(w.relations.size() == 1);
    CHECK(w.relations[0] == "conflict|warned|true");
}

TEST_CASE("world zone bindings are safe no-ops with no host hooks wired (#162)") {
    // A LuaController with no WorldApi at all. The queries must ANSWER rather than error, so a script
    // can branch on them without first asking whether the server has an alert system -- and the
    // answers must be the harmless ones (nobody is at war, nobody is in trouble).
    RecordingWorld w;
    w.api.getAlertLevel = nullptr;
    w.api.getZoneStage = nullptr;
    w.api.isInZone = nullptr;
    w.api.setAlertLevel = nullptr;
    auto c = makeWorldCtrl("function compute_control(s,t,dt)\n"
                           "  if t == 0 then\n"
                           "    world.set_alert_level('russia', 'war_state')\n"
                           "    world.set_relationship(world.get_alert_level('russia'),\n"
                           "                           world.get_zone_stage(7, 'capital'),\n"
                           "                           tostring(world.is_in_zone(7, 'capital')))\n"
                           "  end\n"
                           "  return {}\n"
                           "end\n",
                           &w.api);
    REQUIRE(c->isValid());
    (void)c->sample(EntityState{}, 0, 1.0 / 60.0);

    CHECK(w.alerts.empty());
    REQUIRE(w.relations.size() == 1);
    CHECK(w.relations[0] == "peacetime|clean|false");
}
