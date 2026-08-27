// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <string_view>

// Compiled-in Lua AI scripts for the zero-content-pack sandbox (#866), mirroring BuiltinWeapon /
// BuiltinSensors / BuiltinEntities: scripted AI (`--ai lua <name>`, or an EntityDef's
// aiScriptAsset) must work with no pack mounted, and this is where the reference scripts live. They
// are resolved through the SAME loadAIScript seam as pack scripts, checked first (the "builtin:"
// namespace cannot collide with a pack id).
//
// There are two, because employment geometry is a ROLE, not a setting (#1339): `builtin:fighter`
// flies air-to-air cones, `builtin:striker` flies a dive attack on surface targets. One script that
// tried to do both would have to decide which it was on every tick anyway, and the sandbox ships
// both kinds of target.
//
// Both are honest-sensing by construction: they target ONLY through detected_contacts(), never
// nearby_entities()/get_entity(), so they hunt with the sensors they have and fire INTENTS the
// server re-validates exactly as it does a player's. Station numbers match builtin:debug-entity's
// hardpoints (0 = cannon, 1 = IR rail, 4 = bomb, 5 = rocket pod). They are geodesy-honest too:
// altitude is guidance.altitude(pos), never pos.y, which is an altitude only near the world origin —
// in an `anchor:` mission (the shipped builtin:sandbox included) pos.y is millions of metres
// negative and the pos.y shorthand flew every builtin AI into the terrain (#1221).
namespace fl {

namespace detail {

// Everything both reference scripts share: the geodesy helpers, the #1141/#1143 control laws, the
// hard-deck recovery and the patrol orbit. Concatenated ahead of a role body (below) to form one
// chunk, so its top-level locals are in scope for the body — which is also why the split is a plain
// string concatenation rather than a require(): a builtin script has no pack root to require from.
inline constexpr std::string_view kBuiltinAiPrelude = R"lua(
-- Shared prelude for the builtin AI scripts (#866/#1339). Deliberately conservative gains -- the
-- builtin trainer (#1334) has no FBW G limiter, and unlike the pre-#1334 UFO it has a real stall at
-- 15 deg and real +-7 g structural limits that over-G billing enforces, so a script must respect
-- both itself. It also has no afterburner deck (ctrl.afterburner is a no-op on it). Uses no
-- require(), so it needs no pack root.
-- HARD DECK. A height above TERRAIN (#1352), not an MSL altitude: the ground is not at sea level,
-- and this test used to be `altitude < 600` against a site whose terrain sits at ~545 m -- 55 m of
-- protection, flickering on and off as the aircraft crossed 600 m, and over any terrain higher than
-- 600 m the condition is only true BELOW GROUND, so the recovery was not late, it was unreachable.
-- Measured on demo-sam-strike before the fix: the CAP fought between 547 and 616 m MSL, scraped the
-- terrain (hp 100 -> 77) and ejected at -0 m AGL.
local FLOOR_AGL  = 450         -- below this height above the ground, recovery outranks everything
local FLOOR_MSL  = 600         -- fallback deck when the tick evaluated NO ground reference (nil is
                               -- "no reference", never "sea level" -- reading it as 0 is the defect)
local STALE_S    = 8.0         -- a contact older than this is not worth steering at

-- Employment stations match builtin:debug-entity's hardpoints.
local GUN_STATION     = 0
local MISSILE_STATION = 1
local BOMB_STATION    = 4
local ROCKET_STATION  = 5

local ROLL_GAIN = 2.0          -- aileron per rad of bank error in the hard-deck recovery
local COMBAT_BANK = 1.396      -- 80 deg (kCombatBankRad): pursuit may bank harder than the 45 deg nav default
local STRIKE_BANK = 1.047      -- 60 deg (kFormationBankRad): a strike run-in rolls hard, not vertically

-- ENERGY (#1353). These scripts had no notion of their own, and it killed them. COMBAT_BANK is a
-- CEILING, never a command: a bank angle is a load factor (80 deg = 5.8 g) and the trainer cannot
-- reach that below ~130 m/s or sustain it at any speed on T/W 0.32, so pulling it at 90 m/s buys
-- heading with airspeed that cannot be repaid. Measured on demo-sam-strike: 73-166 m/s across one
-- engagement, then ~30 s oscillating 547 <-> 616 m MSL at 74-88 m/s with the deck flickering, a
-- terrain scrape (hp 100 -> 77), and KIA at -0 m AGL. guidance.bank_limit_for_speed does the sizing.
local RECOVER_SPD  = 160       -- above this a firm pull is affordable; below it, unload (kDeckRecoverSpeedMps)
local HOLD_CLIMB   = 50        -- the gentle climb the low-energy recovery asks for (kDeckHoldClimbM)
local DISENGAGE_SPD = 95       -- slower than this in a fight and there is nothing left to fight WITH
local REENGAGE_SPD  = 145      -- and this is the speed to come back at: near the trainer's cruise

local function len2(x, z) return math.sqrt(x * x + z * z) end

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

-- Local "up": the radial from the planet centre {0, -R, 0} (flight/Geodetic.h). It is +Y only near
-- the world origin -- at the anchored sandbox home it points 36 degrees off the world Y axis, so
-- NOTHING in this script may read pos.y as altitude (#1221): there it is about -2,604,000 m and
-- every altitude comparison against it is nonsense.
local function up_of(p)
  local ux, uy, uz = p.x, p.y + 6371000.0, p.z
  local un = math.sqrt(ux * ux + uy * uy + uz * uz)
  return ux / un, uy / un, uz / un
end

-- Bank angle about the nose relative to the local horizon; positive = right wing down.
-- quat is {x,y,z,w}; engine body axes are +X fwd, +Y up, +Z right.
local function bank_of(state)
  local q, p = state.quat, state.pos
  local ux, uy, uz = up_of(p)
  local byx = 2 * (q.x * q.y - q.w * q.z)          -- body-up in world
  local byy = 1 - 2 * (q.x * q.x + q.z * q.z)
  local byz = 2 * (q.y * q.z + q.w * q.x)
  local bzx = 2 * (q.x * q.z + q.w * q.y)          -- body-right in world
  local bzy = 2 * (q.y * q.z - q.w * q.x)
  local bzz = 1 - 2 * (q.x * q.x + q.y * q.y)
  return math.atan(-(ux * bzx + uy * bzy + uz * bzz), ux * byx + uy * byy + uz * byz)
end

-- Cosine off the nose to a world point, plus 3-D range. Body-forward so pitch counts, not just heading.
local function boresight(state, p)
  local f = guidance.body_forward(state.quat)
  local dx, dy, dz = p.x - state.pos.x, p.y - state.pos.y, p.z - state.pos.z
  local d = math.sqrt(dx * dx + dy * dy + dz * dz)
  if d < 1 then return 1.0, 0 end
  return (f.x * dx + f.y * dy + f.z * dz) / d, d
end

-- Flight-path angle [rad], positive = climbing: where the aircraft is GOING, as opposed to where it
-- is pointing. The difference is the angle of attack, and for an unguided store it is the difference
-- between a hit and a miss (#1339) -- see velsight.
local function fpa_of(state)
  local ux, uy, uz = up_of(state.pos)
  local vx, vy, vz = state.vel.x, state.vel.y, state.vel.z
  local sp = math.sqrt(vx * vx + vy * vy + vz * vz)
  if sp < 1 then return 0 end
  return math.asin(clamp((vx * ux + vy * uy + vz * uz) / sp, -1, 1))
end

-- Cosine off the VELOCITY VECTOR to a world point, plus 3-D range -- the aiming reference for an
-- unguided store. A rocket leaves the rail with the AIRFRAME'S OWN VELOCITY plus a 15 m/s separation
-- push along the nose (ProjectileSystem::launch), so at 150 m/s it departs within a degree of the
-- flight path, NOT along the nose the pilot is looking down. Firing on a boresight solution instead
-- puts every rocket low by the angle of attack: measured on the trainer in a 25 deg dive, ~5 deg,
-- which at 2 km is a 175 m miss.
local function velsight(state, p)
  local vx, vy, vz = state.vel.x, state.vel.y, state.vel.z
  local sp = math.sqrt(vx * vx + vy * vy + vz * vz)
  local dx, dy, dz = p.x - state.pos.x, p.y - state.pos.y, p.z - state.pos.z
  local d = math.sqrt(dx * dx + dy * dy + dz * dz)
  if sp < 1 or d < 1 then return 1.0, d end
  return (vx * dx + vy * dy + vz * dz) / (sp * d), d
end

-- own_alt: MSL altitude of the ownship, computed once per tick in compute_control. talt is MSL too.
local own_alt = 0
-- own_ground: terrain elevation MSL under the ownship this tick, or nil when the caller evaluated
-- none (a unit test, a hand-built context). own_agl and deck_msl follow from it.
local own_ground = nil
local own_agl = nil
local deck_msl = FLOOR_MSL

-- Inner-loop pitch damping for the altitude cascade (#1196), differentiated across our own sample
-- interval exactly like the C++ controllers -- EntityState carries no body rates on either side of
-- the seam. Updated once per tick in compute_control.
local prev_pitch = nil
local pitch_rate = 0
local own_pitch = 0

-- One call at the top of every compute_control: the per-tick state both roles need.
local function begin_tick(state, dt)
  own_alt = guidance.altitude(state.pos)
  -- The radar altimeter (#1352). nil = no ground reference this tick, and the deck falls back to the
  -- old MSL altitude rather than to a terrain height of zero.
  own_ground = guidance.ground_elevation()
  own_agl = own_ground and (own_alt - own_ground) or nil
  deck_msl = own_ground and (own_ground + FLOOR_AGL) or FLOOR_MSL
  own_pitch = guidance.pitch_of(state.quat, state.pos)
  pitch_rate = (prev_pitch and dt > 0) and (own_pitch - prev_pitch) / dt or 0
  prev_pitch = own_pitch
end

-- The three #1141/#1143 laws, adopted here in #1334. The pre-#1334 forms held station only on the
-- old neutrally-stable UFO: rate-only aileron wound the bank up for as long as a heading error
-- survived (the orbit's normal condition), the attitude-based pitch law's P-only elevator drooped
-- below the trim a statically stable airframe needs (the #1186 mechanism), and the aileron-tied
-- rudder commanded nothing in a steady turn. The trainer exposes all three.
-- Own airspeed. Used often enough, and by enough of the energy logic, to be worth naming.
local function speed_of(state)
  local v = state.vel
  return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
end

local function steer(state, tx, tz, talt, throttle, max_bank)
  local herr = guidance.heading_error(state.quat, state.pos, { x = tx, y = state.pos.y, z = tz })
  -- max_bank is the ROLE ceiling; what is actually commanded is the hardest turn this airspeed will
  -- pay for (#1353). At cruise the two are nearly the same; slow, the limit is what stops the script
  -- turning itself onto the back of the drag curve.
  local ail  = guidance.turn_aileron(state.quat, state.pos, herr,
                                     nil, guidance.bank_limit_for_speed(speed_of(state), max_bank or 0.785))
  return {
    aileron     = ail,
    rudder      = guidance.rudder_to_coordinate(guidance.sideslip(state.quat, state.vel)),
    elevator    = guidance.elevator_for_altitude_hold(state.quat, state.pos, state.vel, talt,
                                                      nil, nil, pitch_rate),
    throttle    = throttle,
  }
end

-- Terrain does not negotiate. Returns a recovery control when we are under the hard deck, else nil.
-- Recover in order: wings, THEN pull (#1141). A firm pull while rolled past vertical is a split-S
-- into the terrain, so level the lift vector first and gate the pull on it pointing up. The pull is
-- speed-scaled (#1334): the trainer trims ~0.8 rad of alpha per rad of elevator (cm_de/cm_alpha with
-- 20 deg of travel), so a fixed large pull is over the +7 g structural limit past ~140 m/s and past
-- the 15 deg stall below it. 5300/V^2 holds the pull near 6 g at sea level; the low clamp keeps a
-- slow recovery honest, the high clamp (~14 deg equilibrium alpha) stays under the stall.
--
-- MSL altitude, never pos.y -- read as pos.y this test is permanently true in an anchored mission
-- and the recovery branch never exits (#1221).
local function pull_out(state, throttle)
  local bank = bank_of(state)
  local spd  = speed_of(state)
  -- WINGS, then ENERGY, then pull (#1141/#1353). The speed decides which recovery this is:
  --   * rolled past knife-edge -- no g at all until the lift vector points up again;
  --   * at or above RECOVER_SPD -- the firm speed-scaled pull, which is the dive arrest #1339 sized
  --     (the altitude cascade cannot stop a 30 deg descent at 175 m/s; measured, it hit the terrain
  --     at ~100 m AGL);
  --   * below it -- UNLOAD AND ACCELERATE. The pull is what was holding the aircraft down: at
  --     80 m/s the 5300/V^2 term clamps to its 0.30 maximum, a high-AoA pull at a speed where
  --     induced drag exceeds full power, and the aircraft mushes. Asking the cascade for a gentle
  --     climb instead gives a command built from the CURRENT flight path plus a bounded AoA, which
  --     when mushing is nose-DOWN, so the wing flies again. Measured from 80 m/s at 100 m AGL over
  --     60 s: the old always-pull law ended at 58 m/s -- below the 1 g stall -- after porpoising
  --     0..461 m AGL; this ends at 156 m/s and 316 m AGL, climbing out.
  -- Throttle was never the missing part; it was already 1.0. A mush is escaped with energy.
  local el
  if math.cos(bank) <= 0.5 then
    el = 0.0
  elseif spd >= RECOVER_SPD then
    el = clamp(5300 / math.max(spd * spd, 1), 0.10, 0.30)
  else
    el = guidance.elevator_for_altitude_hold(state.quat, state.pos, state.vel, own_alt + HOLD_CLIMB,
                                             nil, nil, pitch_rate)
  end
  return {
    aileron  = clamp(-ROLL_GAIN * bank, -1, 1),
    rudder   = guidance.rudder_to_coordinate(guidance.sideslip(state.quat, state.vel)),
    elevator = el,
    throttle = throttle or 1.0,
  }
end

local function hard_deck(state)
  if own_alt >= deck_msl then return nil end
  return pull_out(state, 1.0)
end

-- A left-hand orbit around an anchor point; the sensors do the searching.
local function patrol(state, cx, cz, alt, radius)
  local nx, nz = cx - state.pos.x, cz - state.pos.z
  local dc = len2(nx, nz)
  if dc > radius then
    return steer(state, cx, cz, alt, 0.75)
  end
  nx, nz = nx / math.max(dc, 1), nz / math.max(dc, 1)
  local tx = state.pos.x + nx * math.min(dc, 1500) + nz * 2000
  local tz = state.pos.z + nz * math.min(dc, 1500) - nx * 2000
  return steer(state, tx, tz, alt, 0.7)
end

-- Is this contact one we may shoot at, and do we believe it? Faction 0 is neutral.
local function engageable(c, state)
  return c.reacted and c.faction ~= 0 and c.faction ~= state.faction and c.age_s < STALE_S
end

-- Air / ground / naval / structure (#1339). A contact's category is what a sensor operator reads off
-- the scope, and it is the first question any employment decision asks: the air-to-air cones below
-- can never be satisfied against a SAM site, and the dive attack in builtin:striker would be a
-- collision course against an aircraft.
local function is_surface(c)
  return c.category == "ground_vehicle" or c.category == "naval_vehicle" or c.category == "structure"
end
)lua";

// The reference builtin fighter: patrol an orbit, intercept the nearest hostile it has detected, then
// close for guns/IR employment.
inline constexpr std::string_view kBuiltinFighterBody = R"lua(
-- builtin:fighter (#866) -- honest-sensing fighter AI, zero content pack.
local patrol_cx, patrol_cz = nil, nil
local PATROL_ALT = 3000
local PATROL_R   = 6000
local ENGAGE_M   = 2500

local GUN_RANGE_M  = 900
local GUN_CONE_COS  = 0.9962   -- cos(5 deg)
local MSL_RANGE_M  = 9260
local MSL_MIN_M     = 560
local MSL_CONE_COS  = 0.9397   -- cos(20 deg)

-- Nearest hostile AIR contact the pilot has actually noticed and can still trust. Surface contacts
-- are skipped rather than chased (#1339): this script's whole employment geometry is a cone it can
-- only satisfy against something in the air, so hunting a SAM site with it is a way to be shot down
-- while never taking a shot. builtin:striker is the script for those.
local function pick_target(state)
  local best, best_d = nil, 1e30
  for _, c in ipairs(detected_contacts()) do
    if engageable(c, state) and not is_surface(c) then
      local d = len2(c.pos.x - state.pos.x, c.pos.z - state.pos.z)
      if d < best_d then best, best_d = c, d end
    end
  end
  return best, best_d
end

-- Have we run ourselves out of energy? Latched, with a wide gap between the two speeds (#1353): a
-- bare threshold chatters, committing and abandoning the fight every few ticks at exactly the speed
-- where neither choice is being flown properly.
local low_energy = false

function compute_control(state, tick, dt)
  if not patrol_cx then patrol_cx, patrol_cz = state.pos.x, state.pos.z end
  begin_tick(state, dt)

  -- Hard deck first: terrain does not negotiate.
  local recover = hard_deck(state)
  if recover then return recover end

  -- DISENGAGE ON ENERGY (#1353). Below DISENGAGE_SPD there is nothing left to fight with: every
  -- turn costs more speed, the guns cone cannot be held, and the recovery that used to be asked to
  -- fix it could not. So stop fighting and go get the energy back -- wings level, full power, and a
  -- shallow climb toward the patrol altitude -- until REENGAGE_SPD. Losing the merge is a decision
  -- a pilot makes; flying into the ground at 69 m/s is not.
  local spd = speed_of(state)
  if low_energy then
    if spd >= REENGAGE_SPD then low_energy = false end
  elseif spd < DISENGAGE_SPD then
    low_energy = true
  end
  if low_energy then
    local out = {
      aileron  = clamp(-ROLL_GAIN * bank_of(state), -1, 1),
      rudder   = guidance.rudder_to_coordinate(guidance.sideslip(state.quat, state.vel)),
      elevator = guidance.elevator_for_altitude_hold(state.quat, state.pos, state.vel,
                                                     math.min(own_alt + HOLD_CLIMB, PATROL_ALT),
                                                     nil, nil, pitch_rate),
      throttle = 1.0,
    }
    -- A free shot is still a shot. What disengaging declines is TURNING for one -- if a target flies
    -- through the gun cone while we are leaving, the trigger costs no energy at all.
    local tgt = pick_target(state)
    if tgt then
      local cosang, rng = boresight(state, tgt.pos)
      if cosang >= GUN_CONE_COS and rng <= GUN_RANGE_M then
        out.weapon_station = GUN_STATION
        out.trigger = true
      end
    end
    return out
  end

  local tgt, dist = pick_target(state)
  if tgt then
    local dx, dz = tgt.pos.x - state.pos.x, tgt.pos.z - state.pos.z
    local d  = math.max(len2(dx, dz), 1)
    local vc = ((state.vel.x - tgt.vel.x) * dx + (state.vel.z - tgt.vel.z) * dz) / d

    if dist > ENGAGE_M then
      -- INTERCEPT: lead pursuit on the last-known state; trust less the older the contact.
      -- Full MIL only -- the trainer has no afterburner deck to gate on closure (#1334).
      local lead = math.min(dist / math.max(vc, 100), 12) * (tgt.age_s < 1 and 1 or 0.4)
      local ax, az = tgt.pos.x + tgt.vel.x * lead, tgt.pos.z + tgt.vel.z * lead
      return steer(state, ax, az, math.max(guidance.altitude(tgt.pos), deck_msl + 200), 1.0, COMBAT_BANK)
    end

    -- ENGAGE: blend pure with lag pursuit so we slide behind rather than overshoot.
    local lag = math.min((ENGAGE_M - dist) / ENGAGE_M, 0.6)
    local ax  = tgt.pos.x - tgt.vel.x * lag * 2.0
    local az  = tgt.pos.z - tgt.vel.z * lag * 2.0
    local out = steer(state, ax, az, math.max(guidance.altitude(tgt.pos), deck_msl + 200), 1.0, COMBAT_BANK)

    -- EMPLOY: geometry decides the weapon; the server's fire control has the final say.
    local cosang, rng = boresight(state, tgt.pos)
    if cosang >= GUN_CONE_COS and rng <= GUN_RANGE_M then
      out.weapon_station = GUN_STATION
      out.trigger = true
    elseif cosang >= MSL_CONE_COS and rng >= MSL_MIN_M and rng <= MSL_RANGE_M then
      out.weapon_station = MISSILE_STATION
      out.release = true
    end
    return out
  end

  -- PATROL: left-hand orbit around the anchor; the sensors do the searching.
  return patrol(state, patrol_cx, patrol_cz, PATROL_ALT, PATROL_R)
end
)lua";

// The reference builtin striker (#1339): the same honest sensing, pointed at the ground. A dive
// attack with the rocket pod, a terrain floor referenced to the TARGET's own elevation, and an
// egress that resets the geometry for another pass.
inline constexpr std::string_view kBuiltinStrikerBody = R"lua(
-- builtin:striker (#1339) -- honest-sensing surface attack, zero content pack.
--
-- Why it exists: builtin:fighter engages whatever hostile its sensors report, including ground
-- emitters, but its employment geometry is pure air-to-air -- it holds altitude at or above
-- the hard deck + 200 and fires only inside a 5 deg gun cone at 900 m or a 20 deg IR cone inside
-- 2,500 m.
-- Against a surface target it can never satisfy either cone before overflight: measured on
-- demo-sam-strike over 600 s, the site survived indefinitely while every attacker was eventually
-- lost. At the old UFO's performance the script sometimes lucked into a steep-slant snapshot; at
-- realistic performance the gap is structural.
--
-- SCOPE. It prosecutes SURFACE targets with the BOMB (station 4), and that is all:
--   * No air-to-air employment. An escort is the answer to a CAP, not a striker that stops to
--     dogfight; it defends itself by leaving.
--   * One bomb, because the rack holds one. After the release it breaks off and goes back to its
--     patrol rather than flying dry passes over a live site.
--   * No ROCKET employment, deliberately. A rocket is a POWERED store, and the impact-point solver
--     the release run is aimed with (guidance.ccip) models an unpowered one -- so a rocket pass
--     would have to guess its own gravity drop, which at 1.5 km is 18 m against a 10 m lethal
--     radius. Rockets wait on a powered-store solution; a guess dressed as a firing solution is
--     what this issue was about.
local anchor_cx, anchor_cz = nil, nil
local PATROL_ALT = 2500
local PATROL_R   = 6000

local INGRESS_ALT   = 2200     -- run-in altitude, MSL
local ATTACK_AGL_M  = 600      -- height above the TARGET's own elevation for the release run
local ROLL_IN_MIN_M = 2500     -- roll-in range, clamped so a very low or very high ingress still
local ROLL_IN_MAX_M = 9000     -- turns in at a range the airframe can fly the pass from
local NOMINAL_DIVE  = 0.44     -- 25 deg: the dive flown to get down to it. Shallow on purpose --
                               -- the trainer tracks a 25 deg dive to within a degree, and
                               -- steepening it to 45 (tried, measured) put the run-in into a
                               -- rolling scramble the loop could not hold, with the velocity
                               -- vector 70 deg off the target. A pass you can fly beats a pass
                               -- that looks more like dive bombing.
local MIN_DIVE_RAD  = -0.087   -- 5 deg: shallower than this and we are not on a run
local MAX_DIVE_RAD  = -0.61    -- 35 deg: steeper and the pull-off costs more than the pass buys
local DIVE_GAIN     = 2.2      -- elevator per rad of pointing error (see dive_at)
local DIVE_DAMP     = 0.20
local DIVE_AZ_GAIN  = 3.0      -- bank command per rad of bearing error during the run-in
local AIM_AZ_GAIN   = 4.0      -- and per rad of impact-point error on the release run
local BOMB_MISS_M   = 25       -- release when the SOLUTION lands this close: the bomb does 300 at
                               -- the centre falling off linearly to zero at 60 m, so 25 m kills
                               -- anything the zero-pack sandbox puts on the ground
local BOMB_MIN_M    = 700      -- never release inside our own 60 m blast plus the pull-off
local ABORT_ABOVE_M = 220      -- pull off this far above the target's elevation, whatever else
local EGRESS_M      = 7000     -- extend to here before turning back in
local RECOMMIT_ALT  = 1800     -- and be at least this high, MSL
local MEMORY_S      = 45       -- how long a target stays prosecutable after the sensors lose it

local phase = "search"
local bombs_gone = false   -- one bomb per rack: after the release there is no second pass to fly
-- Where the target was when we last actually saw it, and how long ago (#1339). A strike target is a
-- PLACE: an eyeball's +-90 deg scan loses a ground site the moment the run-in passes it, and without
-- a memory the script pulled off, forgot the site existed, and went back to patrolling with the
-- rockets still on the rail -- measured, that is one pass per sortie and no second chance. Nothing
-- here is a wallhack: the position came from this entity's own sensors, and it expires.

-- Nearest hostile SURFACE contact. Same honesty as the fighter: detected, reacted, fresh.
local memory = nil   -- {x, z, alt, age}

local function pick_surface(state, dt)
  local best, best_d = nil, 1e30
  for _, c in ipairs(detected_contacts()) do
    if engageable(c, state) and is_surface(c) then
      local d = len2(c.pos.x - state.pos.x, c.pos.z - state.pos.z)
      if d < best_d then best, best_d = c, d end
    end
  end
  if best then
    memory = { x = best.pos.x, y = best.pos.y, z = best.pos.z,
               alt = guidance.altitude(best.pos), age = 0 }
  elseif memory then
    memory.age = memory.age + dt
    if memory.age > MEMORY_S then memory = nil end
  end
  if not memory then return nil, 0 end
  return memory, len2(memory.x - state.pos.x, memory.z - state.pos.z)
end

-- Fly the FLIGHT PATH down at a point, at a bounded dive angle. The altitude cascade cannot do this:
-- it commands a descent toward a HEIGHT, bounded and gentle, which is right for holding a station
-- and cannot put a run-in where it needs to be. The cascade goes back to work the moment the dive
-- levels off at the release height.
local function dive_at(state, tx, tz, target_alt, dist_h)
  local herr = guidance.heading_error(state.quat, state.pos, { x = tx, y = state.pos.y, z = tz })
  local want = clamp(math.atan(target_alt - own_alt, math.max(dist_h, 1)), MAX_DIVE_RAD, MIN_DIVE_RAD)
  -- DIVE_GAIN, not guidance.elevator_from_pitch_error's 2/pi. That gain is sized for a loop whose
  -- command sits near trim; a dive asks a statically stable airframe to hold its nose well BELOW
  -- trim, and a P-only 2/pi loop simply droops -- measured, the aim sat 4.7 deg high for the whole
  -- run-in, a 160 m error at 2 km. The trainer trims ~0.8 rad of alpha per rad of elevator, so ~2.2
  -- per rad of error is what holds the attitude; the rate term is what stops it ringing.
  --
  -- And it is closed on the FLIGHT PATH, not the pitch attitude: a store leaves with the aircraft's
  -- velocity, so an attitude-closed dive is low by the angle of attack -- another ~5 deg.
  return {
    aileron  = guidance.turn_aileron(state.quat, state.pos, clamp(DIVE_AZ_GAIN * herr, -0.7, 0.7), nil, STRIKE_BANK),
    rudder   = guidance.rudder_to_coordinate(guidance.sideslip(state.quat, state.vel)),
    elevator = clamp(DIVE_GAIN * (want - fpa_of(state)) - DIVE_DAMP * pitch_rate, -0.6, 0.6),
    throttle = 0.85,
  }
end

function compute_control(state, tick, dt)
  if not anchor_cx then anchor_cx, anchor_cz = state.pos.x, state.pos.z end
  begin_tick(state, dt)

  local recover = hard_deck(state)
  if recover then phase = "pullup"; return recover end

  local tgt, dist = pick_surface(state, dt)
  if not tgt then
    phase = "search"
    return patrol(state, anchor_cx, anchor_cz, PATROL_ALT, PATROL_R)
  end

  -- Two different heights, and conflating them is how a pass ends in a hill (#1352). tgt_agl is
  -- height above the TARGET's own elevation -- a thing on the ground tells you the ground height
  -- UNDER IT -- and it is the right reference for every part of the pass geometry, because the
  -- release height is a height above the thing being bombed. `clear` is height above whatever is
  -- closest below us right now: the target's elevation, or the real terrain under the aircraft when
  -- a ground reference was evaluated (guidance.ground_elevation, #1352). Only the safety decisions
  -- -- when to stop pulling off, and whether we are high enough to commit -- use `clear`.
  local talt    = tgt.alt
  local tgt_agl = own_alt - talt
  local clear   = own_agl and math.min(tgt_agl, own_agl) or tgt_agl

  if phase == "pullup" then
    -- A dive is left with a FIRM pull, not by handing the aeroplane back to the altitude cascade:
    -- the cascade commands a bounded climb rate through a bounded AoA, which is right for holding a
    -- station and far too gentle for arresting a 30 deg descent at 175 m/s. Measured with the
    -- cascade doing the recovery, both strikers flew into the terrain at ~100 m AGL on the first
    -- pass; the same pull the hard-deck recovery uses stops it ~250 m higher. Wings first, then pull
    -- (#1141) -- a firm pull while rolled past vertical is a split-S into the ground.
    if own_pitch > 0.17 and clear > ABORT_ABOVE_M then phase = "egress" end
    return pull_out(state, 1.0)
  end

  if phase == "egress" then
    -- Extend away from the site and rebuild the altitude for another pass. Aiming at the point
    -- diametrically opposite keeps the turn away from the threat rather than across it.
    local ax = tgt.x + (state.pos.x - tgt.x) * 3.0
    local az = tgt.z + (state.pos.z - tgt.z) * 3.0
    if dist > EGRESS_M and own_alt > RECOMMIT_ALT then phase = "ingress" end
    return steer(state, ax, az, INGRESS_ALT, 1.0, STRIKE_BANK)
  end

  -- Bombs gone: there is nothing left to prosecute a site WITH, so stop pretending. A dry pass over
  -- a live SAM costs an aeroplane and buys nothing. The pull-off and the egress above still run to
  -- completion first -- breaking off mid-recovery is how two aircraft flying the same script end up
  -- in the same piece of sky.
  if bombs_gone then
    phase = "search"
    return patrol(state, anchor_cx, anchor_cz, PATROL_ALT, PATROL_R)
  end

  if phase ~= "attack" then
    -- Roll in where the GEOMETRY says, not at a fixed range: the dive angle a pass gets is set by
    -- how high you are when you turn in, and a fixed roll-in range gives a 16 deg descent from one
    -- height and a 40 deg one from another. Measured with a fixed 5 km roll-in, the run-in reached
    -- the pull-off altitude while still 2 km out and never entered the firing window at all.
    local roll_in = clamp((tgt_agl - ATTACK_AGL_M) / math.tan(NOMINAL_DIVE) + 2500,
                         ROLL_IN_MIN_M, ROLL_IN_MAX_M)
    phase = (dist < roll_in and clear > ABORT_ABOVE_M) and "attack" or "ingress"
  end

  if phase == "ingress" then
    return steer(state, tgt.x, tgt.z, INGRESS_ALT, 1.0, STRIKE_BANK)
  end

  -- ATTACK. Two parts, because a slow aeroplane cannot dive-bomb: dive to get DOWN to the release
  -- height, then fly the release LEVEL.
  --
  -- Measured, and this is the whole reason the shape is what it is: a bomb released in a 25 deg dive
  -- at 180 m/s lands SHORT, because the dive spends the fall time going down instead of forward --
  -- 624 m short at a 1.8 km slant, and the solution does not cross the target until ~400 m, which is
  -- inside our own blast radius. From LEVEL flight at 600 m above the target the same bomb falls for
  -- ~11 s and travels ~2.1 km, so the release happens at a standoff the airframe can survive and the
  -- solution is exact rather than lucky.
  local out
  if tgt_agl > ATTACK_AGL_M + 100 then
    out = dive_at(state, tgt.x, tgt.z, talt + ATTACK_AGL_M, dist)
  else
    out = steer(state, tgt.x, tgt.z, talt + ATTACK_AGL_M, 1.0, STRIKE_BANK)
  end
  local cosang, rng = velsight(state, tgt)    -- the remembered WORLD position, y included

  -- The bomb is aimed by PREDICTION, not by pointing: guidance.ccip forward-integrates the same
  -- point-mass model the store will fly and says where it would land if released now. Pointing the
  -- aeroplane and pressing the button computes nothing -- a store leaves on the flight path (a
  -- rocket) or straight down off the rack (a bomb), and then falls for several seconds.
  local imp = guidance.ccip(state, talt)
  local miss = imp and len2(imp.x - tgt.x, imp.z - tgt.z) or 1e9

  -- Steer the SOLUTION onto the target, not the nose (#1339). Nulling the bearing error to the
  -- target leaves a standing cross-track offset -- the loop's own lag plus whatever the wind is
  -- doing -- and the bombing solution inherits it: measured, the impact point came no closer than
  -- 81 m however good the pointing looked, which is a miss on anything smaller than an airfield.
  -- The impact point already contains the aircraft's real velocity, so banking on the angle between
  -- IT and the target closes the only error that actually matters.
  if imp then
    local az = guidance.heading_error(state.quat, state.pos, { x = tgt.x, y = state.pos.y, z = tgt.z })
             - guidance.heading_error(state.quat, state.pos, { x = imp.x, y = state.pos.y, z = imp.z })
    local herr = guidance.heading_error(state.quat, state.pos, { x = tgt.x, y = state.pos.y, z = tgt.z })
    out.aileron = guidance.turn_aileron(state.quat, state.pos, herr + AIM_AZ_GAIN * az, nil, STRIKE_BANK)
  end
  if imp and miss <= BOMB_MISS_M and rng >= BOMB_MIN_M then
    out.weapon_station = BOMB_STATION
    out.release = true          -- edge-triggered for a single store: one press, one bomb
    bombs_gone = true
  end

  -- Terrain-floor discipline: the pull-off is referenced to the ground, never to a fixed MSL number
  -- (#1352) -- `clear` is the lower of the target's own elevation and the real terrain under us.
  if bombs_gone or clear < ABORT_ABOVE_M or rng < BOMB_MIN_M then
    phase = "pullup"
  end
  return out
end
)lua";

inline constexpr std::string_view kBuiltinFighterId = "builtin:fighter";
inline constexpr std::string_view kBuiltinStrikerId = "builtin:striker";

// The composed sources. Each role's chunk is the shared prelude plus its own body; they are built
// once, on first use, and handed out as views. (Composed rather than compile-time concatenated
// because a std::string_view has nothing to concatenate INTO — and the alternative, a second copy
// of 120 lines of flight law per role, is how two scripts drift apart.)
inline const std::string& fighterSource() {
    static const std::string src = std::string(kBuiltinAiPrelude) + std::string(kBuiltinFighterBody);
    return src;
}

inline const std::string& strikerSource() {
    static const std::string src = std::string(kBuiltinAiPrelude) + std::string(kBuiltinStrikerBody);
    return src;
}

} // namespace detail

// The Lua source for a builtin AI script id, or empty if unknown. Callers seed their AI-script cache
// with these (root = "" — builtin scripts use no require()) so `--ai lua builtin:fighter` and an
// EntityDef `aiScriptAsset = "builtin:striker"` both resolve with no pack mounted.
//
// NOT noexcept since #1339: the sources are composed from a shared prelude on first use, and a
// function that can allocate should not claim otherwise.
[[nodiscard]] inline std::string_view builtinAiScript(std::string_view id) {
    if (id == detail::kBuiltinFighterId)
        return detail::fighterSource();
    if (id == detail::kBuiltinStrikerId)
        return detail::strikerSource();
    return {};
}

// Every builtin AI script id, for cache seeding / listing.
[[nodiscard]] inline std::span<const std::string_view> builtinAiScriptIds() noexcept {
    static constexpr std::string_view kIds[] = {detail::kBuiltinFighterId, detail::kBuiltinStrikerId};
    return kIds;
}

} // namespace fl
