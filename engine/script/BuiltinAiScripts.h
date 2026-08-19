// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string_view>

// Compiled-in Lua AI scripts for the zero-content-pack sandbox (#866), mirroring BuiltinWeapon /
// BuiltinSensors / BuiltinEntities: scripted AI (`--ai lua <name>`, or an EntityDef's
// aiScriptAsset) must work with no pack mounted, and this is where the reference script lives. It is
// resolved through the SAME loadAIScript seam as pack scripts, checked first (the "builtin:"
// namespace cannot collide with a pack id).
//
// The reference `builtin:fighter` is honest-sensing by construction: it targets ONLY through
// detected_contacts(), never nearby_entities()/get_entity(), so it hunts with the sensors it has and
// fires INTENTS the server re-validates exactly as it does a player's. Station numbers match
// builtin:debug-entity's hardpoints (slot 0 = cannon, slot 1 = IR rail). It is geodesy-honest too:
// altitude is guidance.altitude(pos), never pos.y, which is an altitude only near the world origin —
// in an `anchor:` mission (the shipped builtin:sandbox included) pos.y is millions of metres
// negative and the pos.y shorthand flew every builtin AI into the terrain (#1221).
namespace fl {

namespace detail {

// The reference builtin fighter: patrol an orbit, intercept the nearest hostile it has detected, then
// close for guns/IR employment. Deliberately conservative gains (the builtin flight model has no G
// limiter to lean on). Uses no require(), so it needs no pack root.
inline constexpr std::string_view kBuiltinFighterLua = R"lua(
-- builtin:fighter (#866) -- honest-sensing fighter AI, zero content pack.
local patrol_cx, patrol_cz = nil, nil
local PATROL_ALT = 3000
local PATROL_R   = 6000
local FLOOR      = 600
local STALE_S    = 8.0
local ENGAGE_M   = 2500

-- Employment stations match builtin:debug-entity's hardpoints.
local GUN_STATION     = 0
local MISSILE_STATION = 1
local GUN_RANGE_M  = 900
local GUN_CONE_COS  = 0.9962   -- cos(5 deg)
local MSL_RANGE_M  = 9260
local MSL_MIN_M     = 560
local MSL_CONE_COS  = 0.9397   -- cos(20 deg)

local ROLL_GAIN = 2.0          -- aileron per rad of bank error in the hard-deck recovery

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

-- Nearest hostile contact the pilot has actually noticed and can still trust.
local function pick_target(state)
  local best, best_d = nil, 1e30
  for _, c in ipairs(detected_contacts()) do
    if c.reacted and c.faction ~= 0 and c.faction ~= state.faction and c.age_s < STALE_S then
      local d = len2(c.pos.x - state.pos.x, c.pos.z - state.pos.z)
      if d < best_d then best, best_d = c, d end
    end
  end
  return best, best_d
end

-- own_alt: MSL altitude of the ownship, computed once per tick in compute_control. talt is MSL too.
local own_alt = 0

local function steer(state, tx, tz, talt, throttle, ab)
  local herr = guidance.heading_error(state.quat, state.pos, { x = tx, y = state.pos.y, z = tz })
  local ail  = guidance.bank_to_turn_aileron(herr)
  local perr = guidance.pitch_error_from_alt(state.quat, state.pos, talt - own_alt)
  return {
    aileron     = ail,
    rudder      = guidance.coordinated_rudder(ail),
    elevator    = guidance.elevator_from_pitch_error(perr),
    throttle    = throttle,
    afterburner = ab or false,
  }
end

function compute_control(state, tick, dt)
  if not patrol_cx then patrol_cx, patrol_cz = state.pos.x, state.pos.z end
  own_alt = guidance.altitude(state.pos)

  -- Hard deck first: terrain does not negotiate. MSL altitude, never pos.y -- read as pos.y this
  -- test is permanently true in an anchored mission and the recovery branch never exits (#1221).
  if own_alt < FLOOR then
    local out = steer(state, patrol_cx, patrol_cz, PATROL_ALT, 1.0, true)
    -- Recover in order: wings, THEN pull (#1141). A firm pull while rolled past vertical is a
    -- split-S into the terrain, so level the lift vector first and gate the pull on it pointing up.
    local bank = bank_of(state)
    out.aileron  = clamp(-ROLL_GAIN * bank, -1, 1)
    out.rudder   = guidance.coordinated_rudder(out.aileron)
    out.elevator = math.cos(bank) > 0.5 and 0.5 or 0.0
    return out
  end

  local tgt, dist = pick_target(state)
  if tgt then
    local dx, dz = tgt.pos.x - state.pos.x, tgt.pos.z - state.pos.z
    local d  = math.max(len2(dx, dz), 1)
    local vc = ((state.vel.x - tgt.vel.x) * dx + (state.vel.z - tgt.vel.z) * dz) / d

    if dist > ENGAGE_M then
      -- INTERCEPT: lead pursuit on the last-known state; trust less the older the contact.
      local lead = math.min(dist / math.max(vc, 100), 12) * (tgt.age_s < 1 and 1 or 0.4)
      local ax, az = tgt.pos.x + tgt.vel.x * lead, tgt.pos.z + tgt.vel.z * lead
      return steer(state, ax, az, math.max(guidance.altitude(tgt.pos), FLOOR + 200), 1.0, vc < 120)
    end

    -- ENGAGE: blend pure with lag pursuit so we slide behind rather than overshoot.
    local lag = math.min((ENGAGE_M - dist) / ENGAGE_M, 0.6)
    local ax  = tgt.pos.x - tgt.vel.x * lag * 2.0
    local az  = tgt.pos.z - tgt.vel.z * lag * 2.0
    local out = steer(state, ax, az, math.max(guidance.altitude(tgt.pos), FLOOR + 200), 1.0, vc < 60)

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
  local nx, nz = patrol_cx - state.pos.x, patrol_cz - state.pos.z
  local dc = len2(nx, nz)
  if dc > PATROL_R then
    return steer(state, patrol_cx, patrol_cz, PATROL_ALT, 0.75)
  end
  nx, nz = nx / math.max(dc, 1), nz / math.max(dc, 1)
  local tx = state.pos.x + nx * math.min(dc, 1500) + nz * 2000
  local tz = state.pos.z + nz * math.min(dc, 1500) - nx * 2000
  return steer(state, tx, tz, PATROL_ALT, 0.7)
end
)lua";

inline constexpr std::string_view kBuiltinFighterId = "builtin:fighter";

} // namespace detail

// The Lua source for a builtin AI script id, or empty if unknown. Callers seed their AI-script cache
// with these (root = "" — builtin scripts use no require()) so `--ai lua builtin:fighter` and an
// EntityDef `aiScriptAsset = "builtin:fighter"` both resolve with no pack mounted.
[[nodiscard]] inline std::string_view builtinAiScript(std::string_view id) noexcept {
    if (id == detail::kBuiltinFighterId)
        return detail::kBuiltinFighterLua;
    return {};
}

// Every builtin AI script id, for cache seeding / listing.
[[nodiscard]] inline std::span<const std::string_view> builtinAiScriptIds() noexcept {
    static constexpr std::string_view kIds[] = {detail::kBuiltinFighterId};
    return kIds;
}

} // namespace fl
