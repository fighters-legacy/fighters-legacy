// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlightInputCollector.h"

#include "CameraInput.h"
#include "console/GameConsole.h"
#include "input/BindingQuery.h"
#include "render/SimRenderBridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fl {

std::optional<fl::MsgClientInput> FlightInputCollector::poll(const fl::SimRenderBridge& /*bridge*/,
                                                             CameraInput& camInput, const GameConsole& console,
                                                             const InputSources& sources, bool uiFocused,
                                                             bool textEntry) {
    m_weaponFired = false;

    // The client input SEND CADENCE — deliberately fixed, and the one 1/60 in this file that is not
    // a frame-rate assumption (#1241). It is a rate against a real clock: the server steps at 60 Hz,
    // so sending faster wastes bandwidth on inputs one tick will coalesce, and sending slower drops
    // resolution the server could have used. It is capped, not assumed: below 60 fps the poll simply
    // happens less often, and `elapsed` below reports the truth either way.
    const auto now = m_clock->now();
    // The FIRST poll of a session has no previous timestamp to subtract, so it bills nothing: no
    // time has passed that a rate could act over. Without this it would subtract the clock's epoch
    // and bill the whole clamp — pressing throttle-up on the opening frame would jump it 25%.
    const float elapsed = m_haveLastInput ? std::chrono::duration<float>(now - m_lastInputTime).count() : 0.f;
    if (m_haveLastInput && elapsed < kInputSendPeriodS)
        return std::nullopt;
    m_lastInputTime = now;
    m_haveLastInput = true;

    fl::MsgClientInput inp;
    inp.seqNum = m_inputSeq++;
    // tickIndex + ackMask (the snapshot ack) are stamped by ClientNetEventHandler::stampAck() at the
    // send site (#566) — the net handler is the single ack authority. Left default (0) here.

    // Throttle ramp rate, PER SECOND, multiplied by the time actually elapsed. It used to be a flat
    // 1/60 per accepted poll (#1241), which is a full-travel ramp in one second only while polls
    // land at 60 Hz. Below that they land at the frame rate, so on a 30 fps machine the throttle
    // ramped at HALF speed — a control that responded differently depending on the GPU. The first
    // poll of a session has no previous timestamp, so its elapsed is clamped to one period.
    constexpr float kThrottleRatePerS = 1.0f;
    const float throttleStep = kThrottleRatePerS * std::min(elapsed, kMaxInputElapsedS);
    if (!console.isOpen()) {
        // EVERY control below resolves through the binding table (#1050). There is no raw
        // `isKeyDown(Key::…)` left in this file: a control the table does not own is a control the
        // player cannot rebind and the conflict checker cannot see, which is exactly how master arm
        // ended up hardcoded to V while the radio push-to-talk default was also V.
        //
        // A text field (the chat box, #646) owns the KEYBOARD and the pointer that clicks its Send
        // button, but not the stick — so `suppressDesktop` drops the keyboard and mouse slots and
        // leaves the gamepad and HOTAS axes live, and a partner keeps flying while you type. The
        // throttle holds because ThrottleUp/Down/Max simply read as not-pressed.
        auto down = [&](fl::InputAction a) {
            return fl::actionDown(sources, m_bindings, a, /*suppressDesktop=*/textEntry);
        };
        // Discrete controls an in-flight overlay consumes while it is up (the radio menu, #610).
        // The flight axes, throttle, afterburner, wheel brakes and the gun trigger deliberately stay
        // live — a combat radio menu that idles the engine or safes the guns is worse than no menu.
        auto downUi = [&](fl::InputAction a) { return !uiFocused && down(a); };
        // Rising edge derived from the level state THIS poll observed: poll() runs at 60 Hz and can
        // miss IInput's own one-frame just-pressed flag entirely.
        auto pressed = [&](fl::InputAction a, bool nowDown) { return m_edges.update(a, nowDown); };

        // ── Flight axes ──────────────────────────────────────────────────────
        if (down(fl::InputAction::ThrottleUp))
            camInput.adjustThrottle(throttleStep);
        if (down(fl::InputAction::ThrottleDown))
            camInput.adjustThrottle(-throttleStep);
        inp.throttle = down(fl::InputAction::ThrottleMax) ? 1.f : camInput.throttle();
        inp.elevator = (down(fl::InputAction::PitchUp) ? 1.f : 0.f) + (down(fl::InputAction::PitchDown) ? -1.f : 0.f);
        inp.aileron = (down(fl::InputAction::RollRight) ? 1.f : 0.f) + (down(fl::InputAction::RollLeft) ? -1.f : 0.f);
        inp.rudder = (down(fl::InputAction::YawRight) ? 1.f : 0.f) + (down(fl::InputAction::YawLeft) ? -1.f : 0.f);

        // ── Level flight controls ────────────────────────────────────────────
        if (down(fl::InputAction::FireWeapon))
            inp.buttons |= fl::kInputButtonGun;
        if (down(fl::InputAction::Afterburner))
            inp.buttons |= fl::kInputButtonAfterburner;
        // Wheel brakes (#700). The server ignores them unless the aircraft is on the ground.
        if (down(fl::InputAction::WheelBrake))
            inp.buttons |= fl::kInputButtonWheelBrake;
        // Fire the selected store (#625). Level on the wire — the server's FireControl edge-detects,
        // so holding it is one shot. Gated while an overlay owns the discretes.
        if (downUi(fl::InputAction::FireStore))
            inp.buttons |= fl::kInputButtonFireStore;

        // ── Weapon-station cycling (#625) ────────────────────────────────────
        // Local edge detection, ABSOLUTE selection on the wire (an absolute value converges on a
        // lossy channel; a lost cycle-edge would not). Wraps within the station count Game.cpp
        // provides from the entity def; 0 = unknown (selection off).
        const bool nextEdge = pressed(fl::InputAction::NextWeapon, downUi(fl::InputAction::NextWeapon));
        const bool prevEdge = pressed(fl::InputAction::PrevWeapon, downUi(fl::InputAction::PrevWeapon));
        if (m_stationCount > 0) {
            // 255 = "keep the server's default selection". Only a deliberate cycle replaces it — an
            // untouched selector must never override what the server chose at spawn.
            if ((nextEdge || prevEdge) && m_selectedStation == 255)
                m_selectedStation = 0;
            else if (nextEdge)
                m_selectedStation = static_cast<uint8_t>((m_selectedStation + 1) % m_stationCount);
            else if (prevEdge)
                m_selectedStation = static_cast<uint8_t>((m_selectedStation + m_stationCount - 1) % m_stationCount);
        }

        // ── Radar mode cycle (#526/#528) ─────────────────────────────────────
        // Silent -> Search -> TWS -> STT. Absolute on the wire (converges on the unreliable
        // channel); 255 = keep the server default until the player first presses it, so a fresh
        // spawn stays in its TWS mode.
        if (pressed(fl::InputAction::RadarModeCycle, downUi(fl::InputAction::RadarModeCycle))) {
            m_radarMode = static_cast<uint8_t>((m_radarMode + 1) % 4);
            m_radarModeTouched = true;
        }
        inp.radarMode = m_radarModeTouched ? m_radarMode : 255u;

        // ── Electronic warfare (#529) ────────────────────────────────────────
        // Dispense is a level bit (the server edge-detects, so holding it is one pop); the jammer is
        // a client-side latch sent as level.
        if (downUi(fl::InputAction::CountermeasureDispense))
            inp.buttons |= fl::kInputButtonChaffFlare;
        if (pressed(fl::InputAction::EcmToggle, downUi(fl::InputAction::EcmToggle)))
            m_ecmOn = !m_ecmOn;
        if (m_ecmOn)
            inp.buttons |= fl::kInputButtonEcm;

        // ── Master arm (#641) ────────────────────────────────────────────────
        // Resolved through InputAction::MasterArm like every other control. It used to read
        // `Key::V` directly, so rebinding it did nothing AND it shared a live key with the radio
        // push-to-talk: keying the mic silently safed the guns (#1050). The gate below is applied
        // after every input path, so SAFE really suppresses the fire triggers.
        if (pressed(fl::InputAction::MasterArm, downUi(fl::InputAction::MasterArm)))
            m_masterArm = !m_masterArm;

        // ── Articulation switches (#639) ─────────────────────────────────────
        // Gear toggles, flaps step the detent (clean / manoeuvre / full — the three positions a real
        // lever has; a continuous axis would be a worse keyboard control and matches no cockpit),
        // hook and canopy toggle. All LATCHED and sent as absolute state, so a dropped packet costs
        // a tick of lag, not a sortie in the wrong configuration.
        if (pressed(fl::InputAction::LandingGear, downUi(fl::InputAction::LandingGear)))
            m_gearDown = !m_gearDown;
        if (pressed(fl::InputAction::Flaps, downUi(fl::InputAction::Flaps)))
            m_flapDetent = static_cast<uint8_t>((m_flapDetent + 1u) % 3u);
        if (pressed(fl::InputAction::ArrestorHook, downUi(fl::InputAction::ArrestorHook)))
            m_hookDown = !m_hookDown;
        // The canopy has its own key rather than the old Shift+C chord: the binding table cannot
        // express a chord, and a chord whose modifier is itself a bound action (LeftShift = max
        // throttle) cannot be made to behave.
        if (pressed(fl::InputAction::CanopyToggle, downUi(fl::InputAction::CanopyToggle)))
            m_canopyOpen = !m_canopyOpen;

        // The airbrake is MOMENTARY, held rather than latched — the one control here that is not a
        // switch, and the one place holding it is the natural gesture.
        m_speedbrake = downUi(fl::InputAction::Airbrake) ? 1.f : 0.f;

        // Ejection (#672) and respawn (#648): level on the wire, the server edge-detects, so a held
        // key is one ejection / one request. Gated while an overlay owns the keys.
        if (downUi(fl::InputAction::Eject))
            inp.buttons |= fl::kInputButtonEject;
        if (downUi(fl::InputAction::Respawn))
            inp.buttons |= fl::kInputButtonRespawn;

        m_weaponFired = (inp.buttons & fl::kInputButtonGun) != 0u;

        // ── Analog axes ──────────────────────────────────────────────────────
        // One block for every analog device (#1061). It used to be two: a gamepad block reading the
        // binding table, then a HOTAS block reading four axis INDICES out of `[controls]` in user.toml
        // on device 0 — unrebindable, invisible to the conflict checker, and carrying its own
        // hand-rolled deadzone and invert logic. Now the axes are ordinary bindings and actionAxis()
        // walks the action's list, taking the first one that is actually driving the control. The
        // shipped order puts the joystick first, which reproduces the old "HOTAS wins" precedence,
        // and an `Any` binding reads every connected stick, so a split HOTAS works whichever unit
        // enumerated first (#1358).
        //
        // THE THROTTLE IS A SHARED ACCUMULATOR AND THE LAST MOVER WINS (#1358). The keyboard is a
        // rate (ThrottleUp/Down integrate into camInput above); a lever is a position. An Absolute
        // axis asserts only while it is MOVING — m_axisMotion remembers where every axis sat — so a
        // moving lever snaps the accumulator to its position, a still one leaves the keyboard free to
        // trim from there, and a dead channel stuck at −1.0 never latches the control at idle. The
        // old rule, Absolute = always in command, made any Absolute binding kill the keyboard
        // throttle outright — on a stuck channel, an unflyable aircraft with a clean log.
        auto axis = [&](fl::InputAction a) {
            return fl::actionAxis(sources, m_bindings, m_axisConfig, a, &m_axisMotion);
        };

        if (const fl::AxisSample thr = axis(fl::InputAction::ThrottleAxis); thr.active) {
            camInput.setThrottle(std::clamp(thr.value, 0.0f, 1.0f));
            inp.throttle = camInput.throttle();
        }
        if (const fl::AxisSample elev = axis(fl::InputAction::PitchAxis); elev.active)
            inp.elevator = std::clamp(elev.value, -1.0f, 1.0f);
        if (const fl::AxisSample ail = axis(fl::InputAction::RollAxis); ail.active)
            inp.aileron = std::clamp(ail.value, -1.0f, 1.0f);
        if (const fl::AxisSample rud = axis(fl::InputAction::YawAxis); rud.active)
            inp.rudder = std::clamp(rud.value, -1.0f, 1.0f);
    } else {
        inp.throttle = camInput.throttle();
    }

    // Master-arm gate (#641): SAFE suppresses the gun trigger and fire-store bits on every input
    // path (keyboard, mouse, gamepad, HOTAS), and clears the wasWeaponFired signal. A real safety.
    if (!m_masterArm) {
        inp.buttons &= static_cast<uint8_t>(~(fl::kInputButtonGun | fl::kInputButtonFireStore));
        m_weaponFired = false;
    }

    // Absolute station selection rides every packet (#625): idempotent under loss, converges.
    inp.selectedStation = m_selectedStation;

    // Articulation (#639/#843): absolute state on every packet, for the same reason. The wire carries
    // the COMMAND; the server slews the actuator toward it and the drag follows the position.
    inp.flaps = static_cast<uint8_t>(std::lround(std::clamp(flapCommand(), 0.f, 1.f) * 255.f));
    inp.speedbrake = static_cast<uint8_t>(std::lround(std::clamp(m_speedbrake, 0.f, 1.f) * 255.f));
    inp.artButtons = 0u;
    if (m_gearDown)
        inp.artButtons |= fl::kArtButtonGearDown;
    if (m_hookDown)
        inp.artButtons |= fl::kArtButtonHookDown;
    if (m_canopyOpen)
        inp.artButtons |= fl::kArtButtonCanopyOpen;

    // Camera eye world-position (#858): where this client is looking FROM. The server keys interest
    // management on it for an entity-less peer (an observer ghost camera, or a dead peer), and
    // ignores it for a pilot (the aircraft transform wins). Sent every frame regardless of mode.
    const glm::dvec3 eye = camInput.eyeWorld();
    inp.cameraEye[0] = eye.x;
    inp.cameraEye[1] = eye.y;
    inp.cameraEye[2] = eye.z;
    return inp;
}

void FlightInputCollector::setClock(const fl::IClock& clock) {
    m_clock = &clock;
}

void FlightInputCollector::setBindings(fl::InputBindings bindings) {
    m_bindings = std::move(bindings);
    // A rebind mid-session must not leave a stale edge behind: the old key may have been held when
    // it stopped being this action's binding, which would swallow the next press. Same for the axis
    // motion references — a re-mapped axis's old resting place is not evidence about the new map.
    m_edges.reset();
    m_axisMotion.reset();
}

void FlightInputCollector::setAxisConfig(fl::AxisConfigTable cfg) {
    m_axisConfig = std::move(cfg);
    // A mode change (Centered <-> Absolute) changes what the stored reference means; reseed.
    m_axisMotion.reset();
}

} // namespace fl
