// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlightScreen.h"

#include "CameraInput.h"
#include "ChatOverlay.h"
#include "ClientNetEventHandler.h"
#include "ClientPrediction.h"
#include "CommsMenu.h"
#include "FlightInputCollector.h"
#include "GmMapOverlay.h"
#include "HapticController.h"
#include "IGui.h"
#include "IInput.h"
#include "INetwork.h"
#include "IWindow.h"
#include "InsetViewMath.h" // target-slaved inset camera math (#698)
#include "ManualOverlay.h"
#include "TargetDesignation.h"   // designated-target cycling (#696)
#include "VoiceCommandCapture.h" // voice wingman commands (#935)
#include "config/ControlsSettings.h"
#include "config/UserConfig.h"
#include "console/GameConsole.h"
#include "entity/EntityDef.h"          // EntityDef::name/id for the observer picker label (#860)
#include "entity/EntityTypeRegistry.h" // byIndex
#include "flight/FlightIntegrator.h"   // FlightState (autopilot input, #640)
#include "flight/Geodetic.h"           // kEarthRadiusM
#include "flight/LocalFrame.h"         // bankOf on the local-level frame
#include "input/BindingQuery.h"        // actionDown / actionJustPressed (#689/#1050)
#include "input/InputBindings.h"
#include "net/Capability.h"
#include "render/CameraController.h"
#include "render/FlightHud.h"
#include "render/HudProjection.h" // designator box projection (#696)
#include "render/IHud.h"
#include "render/SceneRenderer.h" // setInsetView (#698)
#include "render/SimRenderBridge.h"
#include "render/TerrainStreamer.h"
#include "render/WindshieldRain.h"
#include "sandbox/SandboxInspector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>
#include <span>
#include <string>

namespace fl {

static const fl::EntityRenderEntry* findEntry(const fl::SimRenderBridge& bridge, uint32_t idx, uint32_t gen) {
    if (!bridge.hasSnapshot())
        return nullptr;
    for (const auto& e : bridge.current().entries)
        if (e.entityIdx == idx && e.entityGen == gen)
            return &e;
    return nullptr;
}

static float rollAngleRad(const fl::EntityRenderEntry* p, double planetRadiusM) {
    if (!p)
        return 0.f;
    // Bank relative to the LOCAL up (radial on a spherical planet), so the windshield lean stays
    // correct far from the world origin (#479). Reduces to atan2(-right.y, up.y) near the origin.
    const float q[4] = {p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w};
    return fl::bankOf(q, p->position, planetRadiusM);
}

FlightScreen::FlightScreen(FlightScreenDeps deps) : m_deps(std::move(deps)) {
    auto& d = m_deps;
    if (d.clientNetHandler && d.prediction && d.env) {
        d.clientNetHandler->snapshotCallback = [pred = d.prediction,
                                                env = d.env](RenderSnapshot& snap, uint64_t tickIndex,
                                                             uint32_t delayTicks, uint32_t ackedSeqNum) {
            pred->reconcile(snap, tickIndex, delayTicks, ackedSeqNum, *env);
        };
    }
}

FlightScreen::~FlightScreen() {
    if (m_deps.clientNetHandler)
        m_deps.clientNetHandler->snapshotCallback = nullptr;
}

Screen FlightScreen::update(IInput& input, IWindow& window) {
    auto& d = m_deps;

    // Every gameplay key below resolves through the binding table (#1050). The exceptions are the
    // MODAL overlays (chat box, radio menu, comms menu, GM map): while one of those is up it raises
    // `uiFocused`/`textEntry`, which suppresses the flight bindings its own Enter/Escape/arrow/digit
    // keys would shadow, so those reads cannot collide by construction — the chat box's Enter/Escape
    // below are the only raw reads left in this file. Anything NON-modal — the aircraft manual, the
    // replay transport, photo mode — is a real binding with its own default.
    const fl::InputBindings* binds = d.inputBindings;
    auto pressed = [&](fl::InputAction a) { return binds && fl::actionJustPressed(input, *binds, a); };
    auto held = [&](fl::InputAction a) { return binds && fl::actionDown(input, *binds, a); };

    uint32_t idx = d.assignedEntityIdx ? *d.assignedEntityIdx : 0;
    uint32_t gen = d.assignedEntityGen ? *d.assignedEntityGen : 0;
    m_playerEntry = findEntry(*d.renderBridge, idx, gen);

    // Remember the last known own position so the dead-pilot ghost camera starts at the wreck (#403).
    if (m_playerEntry)
        m_lastOwnPos = m_playerEntry->position;

    // Observer entity picker (#860): a spectator has no ownship, so the camera views a SELECTED live
    // entity. Num1/Num2 cycle the selection; the first pick jumps the free ghost camera into Chase so
    // the choice is immediately visible. A live pilot always views its own aircraft. #403 extends the
    // spectator state to a DEAD pilot awaiting respawn, not just a role-observer.
    const bool observer = d.clientNetHandler && d.clientNetHandler->grantedRole() == fl::PeerRole::Observer;
    const bool deadSpectator = d.clientNetHandler && d.clientNetHandler->awaitingRespawn();
    // A replay viewer is a spectator by construction: a recording has no ownship, and the picker +
    // free camera the spectator path already provides IS the "view any player's perspective"
    // requirement (#41). Reusing it rather than adding a replay camera mode is the point of D7.
    const bool spectating = observer || deadSpectator || (d.replay != nullptr);
    // On the rising edge into a dead-pilot spectate, drop into the free ghost camera at the wreck (the
    // role-observer path is seeded by Game.cpp's #859 flow instead).
    if (spectating && !m_wasSpectating && deadSpectator && !observer) {
        d.camInput->setFlyEye(m_lastOwnPos);
        d.cameraController->setMode(fl::CameraMode::Free);
    }
    m_wasSpectating = spectating;
    const fl::EntityRenderEntry* viewEntry = m_playerEntry;
    if (spectating) {
        const std::span<const fl::EntityRenderEntry> entries =
            d.renderBridge->hasSnapshot() ? std::span<const fl::EntityRenderEntry>(d.renderBridge->current().entries)
                                          : std::span<const fl::EntityRenderEntry>{};
        if (!d.gameConsole->isOpen() && !(d.wingmanMenu && d.wingmanMenu->isOpen()) &&
            !(d.commsMenu && d.commsMenu->isOpen())) {
            const bool next = pressed(fl::InputAction::SpectateNext);
            const bool prev = pressed(fl::InputAction::SpectatePrev);
            if (next || prev) {
                if (next)
                    m_selector.cycleNext(entries);
                else
                    m_selector.cyclePrev(entries);
                if (d.cameraController->mode() == fl::CameraMode::Free)
                    d.cameraController->setMode(fl::CameraMode::Chase);
            }
        }
        viewEntry = m_selector.resolve(entries);
        // Graceful degrade: the picked entity is gone (destroyed / interest-out / pool-slot reuse), so
        // drop back to the free ghost camera rather than freezing on an empty Chase/Cockpit.
        if (!viewEntry && d.cameraController->mode() != fl::CameraMode::Free)
            d.cameraController->setMode(fl::CameraMode::Free);
    }

    // ── replay transport + photo mode (#41) ─────────────────────────────────
    // Gated on the console/menus being closed, like every other in-flight key handler, so typing a
    // command never scrubs the recording out from under the player.
    if (d.replay && d.replay->isOpen() && !d.gameConsole->isOpen() && !(d.wingmanMenu && d.wingmanMenu->isOpen()) &&
        !(d.commsMenu && d.commsMenu->isOpen())) {
        fl::ReplayPlayer& rp = *d.replay;

        if (pressed(fl::InputAction::ReplayPauseToggle))
            rp.togglePause();
        // Scrub in whole seconds. These share their defaults with the flight controls on purpose:
        // a recording has no ownship, so the Replay context never overlaps the Flight one and the
        // conflict checker treats the reuse as legitimate rather than flagging it (#1050).
        if (pressed(fl::InputAction::ReplaySeekBack))
            rp.seekBySeconds(-5.0);
        if (pressed(fl::InputAction::ReplaySeekForward))
            rp.seekBySeconds(5.0);
        if (pressed(fl::InputAction::ReplaySeekBackFar))
            rp.seekBySeconds(-30.0);
        if (pressed(fl::InputAction::ReplaySeekForwardFar))
            rp.seekBySeconds(30.0);
        if (pressed(fl::InputAction::ReplaySeekStart))
            rp.seekToFraction(0.0);
        if (pressed(fl::InputAction::ReplaySeekEnd))
            rp.seekToFraction(1.0);

        // Speed: the #41 set (0.25x / 0.5x / 1x / 2x), stepped through the shared TimeRate ladder.
        auto stepRate = [](fl::TimeRate r, int dir) {
            constexpr fl::TimeRate kLadder[] = {fl::TimeRate::Quarter, fl::TimeRate::Half, fl::TimeRate::Normal,
                                                fl::TimeRate::Double};
            int step = 2; // Normal
            for (int i = 0; i < 4; ++i)
                if (kLadder[i] == r)
                    step = i;
            step = std::clamp(step + dir, 0, 3);
            return kLadder[step];
        };
        // Speed while playing, exposure inside photo mode — the same two keys. Not a collision:
        // photo mode pauses playback, so a speed control there would adjust nothing, and the
        // ReplaySpeed* actions are scoped to the Replay context while the PhotoEv* ones are scoped
        // to Photo.
        const bool inPhoto = d.photo && d.photo->active;
        if (!inPhoto && pressed(fl::InputAction::ReplaySpeedDown))
            rp.setRate(stepRate(rp.rate(), -1));
        if (!inPhoto && pressed(fl::InputAction::ReplaySpeedUp))
            rp.setRate(stepRate(rp.rate(), +1));

        if (d.photo) {
            // Photo mode pauses on entry: a still of a moving world is a screenshot, not a photograph.
            if (pressed(fl::InputAction::PhotoModeToggle)) {
                d.photo->active = !d.photo->active;
                if (d.photo->active) {
                    if (!rp.paused())
                        rp.togglePause();
                    d.cameraController->setMode(fl::CameraMode::Free);
                }
            }
            if (d.photo->active) {
                const float fovStep = held(fl::InputAction::PhotoFovFine) ? 1.f : 5.f;
                if (held(fl::InputAction::PhotoFovIn))
                    d.photo->fovDeg =
                        std::clamp(d.photo->fovDeg - fovStep, fl::ReplayHud::kMinFovDeg, fl::ReplayHud::kMaxFovDeg);
                if (held(fl::InputAction::PhotoFovOut))
                    d.photo->fovDeg =
                        std::clamp(d.photo->fovDeg + fovStep, fl::ReplayHud::kMinFovDeg, fl::ReplayHud::kMaxFovDeg);
                // Roll is NOT on Q/E: those move the free camera, which is what frames the shot.
                if (held(fl::InputAction::PhotoRollLeft))
                    d.photo->rollDeg = std::clamp(d.photo->rollDeg - 1.f, -180.f, 180.f);
                if (held(fl::InputAction::PhotoRollRight))
                    d.photo->rollDeg = std::clamp(d.photo->rollDeg + 1.f, -180.f, 180.f);
                if (pressed(fl::InputAction::PhotoEvDown))
                    d.photo->evOffset =
                        std::clamp(d.photo->evOffset - 0.25f, -fl::ReplayHud::kMaxEv, fl::ReplayHud::kMaxEv);
                if (pressed(fl::InputAction::PhotoEvUp))
                    d.photo->evOffset =
                        std::clamp(d.photo->evOffset + 0.25f, -fl::ReplayHud::kMaxEv, fl::ReplayHud::kMaxEv);
                if (pressed(fl::InputAction::PhotoCapture) && d.photoCaptureRequest)
                    *d.photoCaptureRequest = true; // Game.cpp owns the renderer; the screen just asks
            }
        }
    }

    // Head tracking (#927): drain the opentrack UDP stream and feed the smoothed pose to the cockpit
    // look before the camera pose is computed.
    if (d.headTracker && d.userConfig) {
        d.headTracker->poll(1.0f / 60.0f, d.userConfig->headTracking());
        d.camInput->setHeadPose(&d.headTracker->pose());
    }

    d.camInput->pollModeKeys(*d.cameraController, *d.gameConsole, input, viewEntry);
    d.camInput->update(*d.cameraController, viewEntry, *d.gameConsole, *d.terrainStreamer, input);

    // Radio menu (#610). Non-modal: the aircraft keeps flying while it is open (see WingmanMenu.h),
    // so only the discrete keys it consumes are suppressed, via FlightInputCollector's uiFocused.
    const bool menuWasOpen = d.wingmanMenu && d.wingmanMenu->isOpen();
    const bool commsMenuWasOpen = d.commsMenu && d.commsMenu->isOpen();
    if (d.wingmanMenu && !d.gameConsole->isOpen() && !commsMenuWasOpen) {
        if (!menuWasOpen && pressed(fl::InputAction::WingmanMenu)) {
            d.wingmanMenu->toggle();
        } else if (menuWasOpen) {
            if (auto order = d.wingmanMenu->update(input); order && d.clientNet) {
                d.clientNet->send(fl::kNetChReliable, &*order, sizeof(*order), /*reliable=*/true);
            }
        }
    }

    // ATC comms menu (#704). Same non-modal contract as the wingman menu — the aircraft keeps flying;
    // only the discrete keys it consumes are suppressed via uiFocused. T toggles it. Mutually exclusive
    // with the wingman menu so their digit keys never collide.
    if (d.commsMenu && !d.gameConsole->isOpen() && !menuWasOpen) {
        if (!commsMenuWasOpen && pressed(fl::InputAction::CommsMenu)) {
            d.commsMenu->toggle();
        } else if (commsMenuWasOpen) {
            if (auto cmd = d.commsMenu->update(input); cmd && d.clientNet) {
                d.clientNet->send(fl::kNetChReliable, &*cmd, sizeof(*cmd), /*reliable=*/true);
            }
        }
    }

    // In-match text chat (#646). Y opens the all channel, H the team channel. While the input box is up
    // it OWNS the keyboard (textEntry below suppresses all flight keys so typing does not fire the gun);
    // the gamepad/HOTAS axes stay live. Send = the Send button or Enter; Escape cancels. Mutually
    // exclusive with the radio/comms menus and the console.
    const bool chatWasOpen = d.chat && d.chat->isInputOpen();
    if (d.chat && !d.gameConsole->isOpen() && !menuWasOpen && !commsMenuWasOpen) {
        if (!chatWasOpen) {
            if (pressed(fl::InputAction::ChatAll))
                d.chat->open(fl::ChatChannel::All);
            else if (pressed(fl::InputAction::ChatTeam))
                d.chat->open(fl::ChatChannel::Team);
        } else {
            const bool sendBtn = d.chat->renderInput(d.gui);
            if (sendBtn || input.isKeyJustPressed(Key::Enter)) {
                if (d.clientNetHandler && !d.chat->text().empty())
                    d.clientNetHandler->sendChat(d.chat->channel(), d.chat->text());
                d.chat->submit();
            } else if (input.isKeyJustPressed(Key::Escape)) {
                d.chat->cancel();
            }
        }
    }
    const bool chatOpen = d.chat && d.chat->isInputOpen();

    // Game-master overview map (#861). M toggles it for a GM-capable peer (typically an observer, so
    // hijacking the arrow keys for pan costs no flight). Non-modal but map-focused: while open, flight
    // input is suppressed via uiFocused. The map's "View from here" button hands a {idx,gen} back here
    // to drive the entity-view camera (the #860 EntitySelector + Chase), and the overlay itself sends a
    // `spectate` so the server re-centres interest on the target.
    const bool gmCapable = d.clientNetHandler && d.clientNetHandler->hasCapability(fl::Capability::GmMap);
    if (d.gmMap && gmCapable && !d.gameConsole->isOpen() && !menuWasOpen && !commsMenuWasOpen && !chatOpen) {
        if (pressed(fl::InputAction::GmMap))
            d.gmMap->toggle();
        if (d.gmMap->isOpen()) {
            d.gmMap->update(input, window);
            if (auto v = d.gmMap->takeViewRequest()) {
                m_selector.select(v->idx, v->gen);
                if (d.cameraController)
                    d.cameraController->setMode(fl::CameraMode::Chase);
            }
        }
    }
    const bool gmMapOpen = d.gmMap && d.gmMap->isOpen();

    // Crew seat picker (#975), non-modal like the radio menu: CrewSeatCycle steps through the
    // joinable seats across every crewed aircraft the client knows, CrewSeatJoin takes the selected
    // one, CrewSeatLeave gives it up. Non-modal means no input is suppressed while it is up, so
    // these need keys of their own — the cluster moved off K/L/U, where K was the airbrake (#1050).
    // Suppressed only while the console/radio menu is up (they own the keyboard then).
    if (d.clientNetHandler && !d.gameConsole->isOpen() && !(d.wingmanMenu && d.wingmanMenu->isOpen()) &&
        !(d.commsMenu && d.commsMenu->isOpen()) && !chatOpen) {
        const bool cycleEdge = pressed(fl::InputAction::CrewSeatCycle);
        const bool joinEdge = pressed(fl::InputAction::CrewSeatJoin);
        const bool leaveEdge = pressed(fl::InputAction::CrewSeatLeave);
        if (cycleEdge) {
            m_seatPicker.rebuild(d.clientNetHandler->crewRosters());
            if (!m_seatPickerActive)
                m_seatPickerActive = true; // first press opens the overlay
            else
                m_seatPicker.next();
        }
        if (joinEdge && m_seatPickerActive) {
            if (const fl::SeatTarget* t = m_seatPicker.selected())
                d.clientNetHandler->sendSeatRequest(t->entityIdx, t->entityGen, t->seatIndex);
        }
        if (leaveEdge)
            d.clientNetHandler->sendSeatLeave();

        // Surface the last MsgSeatResult as a one-line label (client-side strings, localizable).
        if (const auto res = d.clientNetHandler->takeSeatResult(); res.fresh) {
            const char* msg = "seat: ?";
            switch (static_cast<fl::SeatResultCode>(res.code)) {
            case fl::SeatResultCode::Granted:
                msg = res.entityIdx ? "seat: joined" : "seat: left";
                m_seatPickerActive = false;
                break;
            case fl::SeatResultCode::SeatOccupiedByHuman:
                msg = "seat: taken by another player";
                break;
            case fl::SeatResultCode::FlySeatNotJoinable:
                msg = "seat: pilot seat not joinable";
                break;
            case fl::SeatResultCode::NotCrewed:
                msg = "seat: not a crewed aircraft";
                break;
            case fl::SeatResultCode::NoSuchSeat:
                msg = "seat: no such seat";
                break;
            case fl::SeatResultCode::NoSuchEntity:
                msg = "seat: aircraft gone";
                break;
            case fl::SeatResultCode::NotInSeat:
                msg = "seat: you hold no seat";
                break;
            }
            std::snprintf(m_seatResultLine, sizeof(m_seatResultLine), "%s", msg);
        }
    }

    const bool consoleWasOpen = d.gameConsole->isOpen();
    if (consoleWasOpen) {
        if (d.gameConsole->tick(input))
            d.gameConsole->close(input);
    }
    if (!consoleWasOpen && d.gameConsole->isOpen() && d.hapticController)
        d.hapticController->onPause(0);

    // SandboxInspector intercepts Escape; returning false = user requested exit.
    if (d.inspector && !d.inspector->update() && !consoleWasOpen)
        return Screen::MainMenu;

    const ControlsSettings cs = d.userConfig->controls();
    const bool uiFocused =
        (d.wingmanMenu && d.wingmanMenu->isOpen()) || (d.commsMenu && d.commsMenu->isOpen()) || chatOpen || gmMapOpen;

    // Voice wingman commands (#935). Hold the key, speak, release: captured locally, transcribed
    // locally, matched locally, and what leaves the machine is the SAME MsgWingmanCommand the radio
    // menu sends — the pilot's voice never does. `uiFocused` closes the gate for the same reason
    // VoiceChat's does: typing "engage" into chat must not also say it.
    if (d.voiceCommands && d.inputBindings) {
        const bool held =
            fl::actionDown(input, *d.inputBindings, fl::InputAction::WingmanVoiceCommand) && !d.gameConsole->isOpen();
        d.voiceCommands->update(held, uiFocused);
    }

    // Planet radius (m) from the server's MsgConnectAck; drives the autopilot's local-level altitude/
    // heading as well as the HUD attitude/horizon below. Earth default until the ack arrives.
    const double radiusM =
        d.clientNetHandler ? static_cast<double>(d.clientNetHandler->planetRadiusKm()) * 1000.0 : fl::kEarthRadiusM;

    // Player autopilot (#640): edge-detect the hold toggles (Cockpit/Padlock, no overlay/console
    // focus) and disengage everything when there is no predicted ownship state (observer / dead peer).
    const fl::FlightState* fs = d.prediction ? d.prediction->predictedState() : nullptr;
    if (fs && d.inputBindings && !uiFocused && !d.gameConsole->isOpen()) {
        if (pressed(fl::InputAction::AutopilotAltHold))
            m_autopilot.toggleAltHold(*fs, radiusM);
        if (pressed(fl::InputAction::AutopilotHdgHold))
            m_autopilot.toggleHdgHold(*fs, radiusM);
        if (pressed(fl::InputAction::AutopilotSpdHold))
            m_autopilot.toggleSpdHold(*fs);
    } else if (!fs) {
        m_autopilot.disengageAll();
    }

    // Target designation cycling (#696): Cockpit/Padlock, no overlay/console focus. Build the candidate
    // context from the snapshot + the client's category/IFF seams (#688) and cycle on the action edge.
    const bool designateAllowed = d.targetDesignation && d.inputBindings && !uiFocused && !d.gameConsole->isOpen();
    if (designateAllowed && viewEntry && d.renderBridge->hasSnapshot()) {
        // Shared candidate context: category via the type registry, IFF via the #688 client helper.
        auto makeCtx = [&]() {
            fl::DesignationContext dctx;
            dctx.snap = &d.renderBridge->current();
            dctx.ownIdx = viewEntry->entityIdx;
            dctx.ownGen = viewEntry->entityGen;
            dctx.ownPos = viewEntry->position;
            dctx.ownForward = viewEntry->orientation * glm::vec3{1.f, 0.f, 0.f};
            if (d.entityRegistry) {
                fl::EntityTypeRegistry* reg = d.entityRegistry;
                dctx.categoryOf = [reg](uint32_t ti) -> uint8_t {
                    const fl::EntityDef* def = reg->byIndex(ti);
                    return def ? static_cast<uint8_t>(def->category) : 0u;
                };
            }
            if (d.clientNetHandler) {
                fl::ClientNetEventHandler* h = d.clientNetHandler;
                dctx.identOf = [h](const fl::EntityRenderEntry& e) -> uint8_t {
                    return h->identForEntity(e.entityIdx, e.entityGen, e.factionIndex);
                };
            }
            return dctx;
        };

        const bool next = pressed(fl::InputAction::NextTarget);
        const bool prev = pressed(fl::InputAction::PrevTarget);
        if (next || prev)
            d.targetDesignation->cycle(next ? +1 : -1, makeCtx());

        // Padlock toggle (#697): F5 enters padlock (auto-designating best-in-cone when nothing is
        // designated) and exits back to Cockpit; the tracker is seeded from the current view so it
        // never pops. All the slew/lock math lives in CameraInput's Padlock case.
        if (pressed(fl::InputAction::PadlockToggle)) {
            if (d.cameraController->mode() == fl::CameraMode::Padlock) {
                d.cameraController->setMode(fl::CameraMode::Cockpit);
            } else {
                constexpr float kDesignateHalfAngleRad = 0.2618f; // 15 deg
                if (!d.targetDesignation->resolve(d.renderBridge->current()))
                    d.targetDesignation->designateBest(makeCtx(), kDesignateHalfAngleRad);
                if (d.targetDesignation->designated() && d.camInput) {
                    d.camInput->enterPadlock();
                    d.cameraController->setMode(fl::CameraMode::Padlock);
                }
            }
        }

        // Target-slaved inset toggle (#698).
        if (pressed(fl::InputAction::TargetInsetToggle))
            m_insetOn = !m_insetOn;

        // Radar MFD page / range cycling (#642).
        if (pressed(fl::InputAction::MfdPage))
            m_mfd.cyclePage();
        if (pressed(fl::InputAction::MfdRange))
            m_mfd.cycleRange();

        // Night-vision goggles toggle (#210).
        if (pressed(fl::InputAction::NvgToggle))
            m_nvgOn = !m_nvgOn;
    }

    // Feed the padlock view its target each frame (resolve auto-clears on despawn/death). Stored for
    // the HUD box + PADLOCK cue below; also drives CameraInput's Padlock slew next frame.
    m_designatedTarget = (d.targetDesignation && d.renderBridge->hasSnapshot())
                             ? d.targetDesignation->resolve(d.renderBridge->current())
                             : nullptr;
    if (d.camInput)
        d.camInput->setPadlockTarget(m_designatedTarget);

    if (auto msg = d.flightInput->poll(*d.renderBridge, *d.camInput, *d.gameConsole, input, d.joystick, cs, uiFocused,
                                       /*textEntry=*/chatOpen)) {
        // Shape the input with the autopilot BEFORE prediction+send, so the client predicts exactly what
        // the server receives. A player stick/throttle input past threshold disengages the relevant holds.
        if (fs && m_autopilot.modes() != 0) {
            const float rawThrottle = msg->throttle;
            const bool throttleTouched = std::abs(rawThrottle - m_lastRawThrottle) > 0.02f;
            m_autopilot.notePlayerInput(msg->elevator, msg->aileron, msg->rudder, throttleTouched);
            const fl::AutopilotCommand ap = m_autopilot.compute(*fs, 1.0f / 60.0f, radiusM);
            if (ap.hasPitch)
                msg->elevator = ap.elevator;
            if (ap.hasRoll) {
                msg->aileron = ap.aileron;
                msg->rudder = ap.rudder;
            }
            if (ap.hasThrottle) {
                msg->throttle = ap.throttle;
                d.camInput->setThrottle(ap.throttle); // keep the persistent throttle in sync (no snap-back)
            }
            m_lastRawThrottle = msg->throttle;
        } else {
            m_lastRawThrottle = msg->throttle;
        }

        // Stamp the snapshot ack (tickIndex + selective-ack mask, #566) from the net handler — the single
        // ack authority — before prediction and send, so the outgoing input carries a consistent ack.
        if (d.clientNetHandler)
            d.clientNetHandler->stampAck(*msg);
        if (d.prediction && d.env)
            d.prediction->onInput(*msg, *d.env);
        // Null in a replay session (#41): there is no server to fly at. Every other use of the net in
        // this function was already null-guarded; this one send was not, and it was reachable the
        // moment a session existed without a socket.
        if (d.clientNet)
            d.clientNet->send(0, &*msg, sizeof(*msg), /*reliable=*/false);
    }
    if (d.clientNetHandler)
        d.clientNetHandler->sendHeartbeatIfNeeded();
    m_weaponFired = d.flightInput->wasWeaponFired();

    // Terrain elevation above the datum along the radial through the entity (heightAt(dvec3));
    // the HUD/haptics derive radial AGL from it against the geodetic altitude (#477).
    const float terrainElev = viewEntry ? static_cast<float>(d.terrainStreamer->heightAt(viewEntry->position)) : 0.f;
    // Padlock (#697) is a cockpit-eye view: the HUD shows and the ownship is hidden, exactly like Cockpit.
    const fl::CameraMode camMode = d.cameraController->mode();
    const bool cockpit = (camMode == fl::CameraMode::Cockpit || camMode == fl::CameraMode::Padlock);

    // Observer picker label (#860): name + faction of the entity being viewed, shown top-centre. Built
    // here where the registry (type name) and the net handler (faction name) are both reachable.
    m_pickerLabel[0] = '\0';
    if (spectating && viewEntry) {
        const char* typeName = "entity";
        if (d.entityRegistry) {
            if (const fl::EntityDef* def = d.entityRegistry->byIndex(viewEntry->typeIndex))
                typeName = def->name.empty() ? def->id.c_str() : def->name.c_str();
        }
        const std::string faction =
            d.clientNetHandler ? d.clientNetHandler->factionName(viewEntry->factionIndex) : std::string{};
        if (!faction.empty())
            std::snprintf(m_pickerLabel, sizeof(m_pickerLabel), "[ %s  |  %s ]", typeName, faction.c_str());
        else
            std::snprintf(m_pickerLabel, sizeof(m_pickerLabel), "[ %s ]", typeName);
    }

    static constexpr uint32_t kMinLatencyDisplayMs = 5u;
    const uint32_t latencyMs = d.clientNetHandler ? d.clientNetHandler->snapshotLatencyMs() : 0u;
    const bool showLat = d.userConfig->hud().showLatency && d.clientNetHandler &&
                         d.clientNetHandler->hasSnapshotLatency() && latencyMs >= kMinLatencyDisplayMs;

    // radiusM (planet radius, from MsgConnectAck) was computed above the poll block for the autopilot.

    // Ground-crew scene + landing detection (#55), own aircraft only. The airborne→landed edge
    // scores the touchdown from last frame's sink rate into the logbook (PilotLogbook::recordLanding
    // finally gets a producer, #674); two seconds landed-and-stopped blends the Chase camera into a
    // slow ramp orbit. The scene clears on the takeoff roll, a manual mode change away from Chase,
    // or after 60 s — and everything degrades gracefully: no ownship (observer) = no scene.
    if (m_playerEntry && d.camInput) {
        const double ownTerrain = d.terrainStreamer->heightAt(m_playerEntry->position);
        const double ownAlt = fl::geodeticAltitude(m_playerEntry->position.x, m_playerEntry->position.y,
                                                   m_playerEntry->position.z, radiusM);
        const double agl = ownAlt - ownTerrain;
        const float spd = glm::length(m_playerEntry->velocity);
        const bool onGround = agl < 3.0;

        if (m_wasAirborne && onGround && d.userConfig) {
            // Touchdown: score the landing from the sink rate the frame before contact. ~1 m/s is a
            // greaser (100), 6+ m/s is an arrival (0).
            const float sink = std::max(0.f, -m_prevVertSpeedMps);
            const float score = std::clamp(100.f - 20.f * std::max(0.f, sink - 1.f), 0.f, 100.f);
            PilotSettings ps = d.userConfig->pilot();
            ps.profile.logbook.recordLanding(score);
            d.userConfig->setPilot(ps);
        }
        m_wasAirborne = agl > 8.0;
        m_prevVertSpeedMps = m_playerEntry->velocity.y;

        constexpr double kSceneEnterS = 2.0, kSceneTimeoutS = 60.0;
        if (onGround && spd < 1.5f)
            m_landedStillS += 1.0 / 60.0;
        else
            m_landedStillS = 0.0;
        const bool wantScene = m_landedStillS >= kSceneEnterS && m_landedStillS < kSceneEnterS + kSceneTimeoutS;
        if (wantScene && !m_groundSceneOn && d.cameraController->mode() == fl::CameraMode::Chase) {
            m_groundSceneOn = true; // the orbit engages only from the external view — never yanks Cockpit
        }
        if (m_groundSceneOn && (!wantScene || d.cameraController->mode() != fl::CameraMode::Chase))
            m_groundSceneOn = false;
        d.camInput->setGroundScene(m_groundSceneOn);
    } else if (d.camInput) {
        m_groundSceneOn = false;
        d.camInput->setGroundScene(false);
    }

    // Datalink track picture + RWR (#528) for the HUD radar scope — only when this peer flies its own
    // aircraft (a spectator following another entity has no datalink of its own to draw).
    const fl::RadarView radar = (cockpit && d.clientNetHandler) ? d.clientNetHandler->radarView() : fl::RadarView{};

    // Build the HUD input bundle (#438). The camera pose is current (CameraInput::update just ran), so
    // computing the frame's CameraView here is exact — D1 seam: FlightScreen owns the view the HUD's
    // world->screen symbology projects against. m_frameCam is cached for buildElements()-time cues.
    const float aspect = static_cast<float>(window.width()) / static_cast<float>(std::max(1, window.height()));
    m_frameCam = d.cameraController->view(aspect);
    fl::HudFrameInput hin;
    hin.ownship = cockpit ? viewEntry : nullptr;
    hin.camera = m_frameCam;
    hin.cameraValid = cockpit;
    hin.timeOfDay = d.env->timeOfDay;
    hin.terrainElevation = terrainElev;
    hin.latencyMs = latencyMs;
    hin.showLatency = showLat;
    // #576: presence of the server-throttle TLV, latched. Independent of showLatency — a player who
    // turned the latency readout off still needs to know the SERVER is the reason their world is
    // updating slowly, because that is not a thing they can fix.
    hin.serverThrottled = d.clientNetHandler && d.clientNetHandler->serverThrottled();
    hin.serverLoadPct = d.clientNetHandler ? d.clientNetHandler->serverThrottleLoadPct() : 100;
    hin.planetRadiusM = radiusM;
    hin.radar = radar;
    // Radar MFD page state (#642); annunciate the requested radar mode from the collector.
    m_mfd.radarMode = d.flightInput ? d.flightInput->radarMode() : 2;
    hin.mfd = m_mfd;
    // Night-vision goggles (#210): cockpit-only. Publishes the gain to the render loop and the HUD cue.
    const bool nvg = m_nvgOn && cockpit;
    hin.nvgActive = nvg;
    if (d.nvgIntensity)
        *d.nvgIntensity = nvg ? 1.0f : 0.0f;
    // Autopilot annunciation (#640).
    hin.apModes = m_autopilot.modes();
    hin.apTargetAltM = m_autopilot.targetAltM();
    hin.apTargetHeadingDeg = glm::degrees(m_autopilot.targetHeadingRad());
    hin.apTargetSpeedMps = m_autopilot.targetSpeedMps();
    // Designated target (#696): resolved above (auto-clears on despawn/death). Only shown in cockpit.
    hin.designatedTarget = cockpit ? m_designatedTarget : nullptr;
    hin.masterArm = d.flightInput ? d.flightInput->masterArm() : true; // #641
    hin.terrainHeightAt = [ts = d.terrainStreamer](const glm::dvec3& p) {
        return ts ? ts->heightAt(p) : 0.0; // #641 CCIP fall solution
    };
    (*d.activeHud)->update(hin);

    // Target-slaved inset view (#698): a live 3D repeater of the designated target, framed bottom-centre.
    // Built here where the frame's camera + resolved target are available; the renderer draws it via the
    // #695 secondary-camera pass. Auto-hides when the designation clears.
    m_insetActive = false;
    if (d.sceneRenderer) {
        if (m_insetOn && m_designatedTarget) {
            m_insetRect = fl::insetRectFor(aspect);
            const glm::vec3 up = fl::radialUp(m_frameCam.worldOrigin, radiusM);
            const fl::CameraView insetCam = fl::buildTargetInsetView(
                m_designatedTarget->position, m_designatedTarget->velocity, /*renderAlpha=*/0.f, m_frameCam.worldOrigin,
                up, /*rectAspect=*/1.0f, /*standoffM=*/30.0);
            d.sceneRenderer->setInsetView(&insetCam, m_insetRect);
            m_insetActive = true;
        } else {
            d.sceneRenderer->setInsetView(nullptr, glm::vec4{0.f});
        }
    }
    d.windshieldRain->update(cockpit ? (1.f / 60.f) : 0.f, cockpit ? *d.env : EnvironmentState{},
                             cockpit ? rollAngleRad(viewEntry, radiusM) : 0.f);
    // Haptics only for a real ownship — an observer viewing another entity should not feel its hits.
    if (d.hapticController)
        d.hapticController->update(m_playerEntry, m_weaponFired, terrainElev, 1.f / 60.f, radiusM);

    // The in-flight aircraft manual (#821). Non-modal, like the radio menu: the aircraft keeps flying
    // while you read it, because a reference you must stop flying to consult is one you never open.
    const bool manualWasOpen = d.manual && d.manual->isOpen();
    if (d.manual && !d.gameConsole->isOpen() && !menuWasOpen) {
        if (pressed(fl::InputAction::AircraftManual))
            d.manual->toggle();
        if (d.manual->isOpen()) {
            // Its OWN scroll bindings, not PageUp/PageDown: the manual is non-modal, so those keys
            // are still the throttle while it is open (#1050).
            if (pressed(fl::InputAction::ManualScrollDown))
                d.manual->scroll(+10);
            if (pressed(fl::InputAction::ManualScrollUp))
                d.manual->scroll(-10);
            // Back out with the same action that would otherwise pause: the guard below stops it
            // doing both in one frame.
            if (pressed(fl::InputAction::Pause))
                d.manual->close();
        }
        d.manual->update();
    }

    // Escape closed the radio menu, the manual, or the chat box this frame; it must NOT also open the
    // pause screen.
    const bool manualClosedThisFrame = manualWasOpen && d.manual && !d.manual->isOpen();
    if (!menuWasOpen && !commsMenuWasOpen && !manualClosedThisFrame && !chatWasOpen && !consoleWasOpen &&
        !d.gameConsole->isOpen() && pressed(fl::InputAction::Pause))
        return Screen::Pause;

    return Screen::Flight;
}

std::span<const HudElement> FlightScreen::buildElements() {
    m_elementCount = 0;
    const auto hudSpan = (*m_deps.activeHud)->elements();
    const auto rainSpan = m_deps.windshieldRain->elements();

    for (const auto& e : hudSpan) {
        if (m_elementCount >= kMaxElements)
            break;
        m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
    }
    for (const auto& e : rainSpan) {
        if (m_elementCount >= kMaxElements)
            break;
        m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
    }

    // Designated-target box + TGT line (#696). Projected against the frame's camera; tracks the target
    // at 60 Hz using its extrapolated position, and disappears when off-screen or the designation clears
    // (the combat HUD #641 enriches this with IFF colour + closure). Minimal cue here.
    if (m_designatedTarget && m_playerEntry) {
        const glm::dvec3 tpos = m_designatedTarget->position + glm::dvec3(m_designatedTarget->velocity * (1.f / 60.f));
        if (auto p = fl::worldToHud(m_frameCam, tpos);
            p && p->x > 0.02f && p->x < 0.98f && p->y > 0.02f && p->y < 0.98f && m_elementCount + 5 <= kMaxElements) {
            // IFF colour (#641): friend green, foe red, unknown amber — via the #688 client helper.
            float br = 0.9f, bg = 0.9f, bb = 0.2f;
            if (m_deps.clientNetHandler) {
                const uint8_t ident = m_deps.clientNetHandler->identForEntity(
                    m_designatedTarget->entityIdx, m_designatedTarget->entityGen, m_designatedTarget->factionIndex);
                if (ident == fl::kIffFriend) {
                    br = 0.2f;
                    bg = 1.0f;
                    bb = 0.4f;
                } else if (ident == fl::kIffFoe) {
                    br = 1.0f;
                    bg = 0.2f;
                    bb = 0.2f;
                }
            }
            const auto box = fl::hudBox(*p, glm::vec2{0.03f, 0.03f}, br, bg, bb, 1.0f, 1.5f);
            for (const auto& e : box)
                m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
            // Range + closure (Vc = -d(range)/dt along the LOS, kt): positive = closing.
            const glm::dvec3 los = m_designatedTarget->position - m_playerEntry->position;
            const double rngM = glm::length(los);
            const double rngKm = rngM / 1000.0;
            float closureKt = 0.f;
            if (rngM > 1.0) {
                const glm::vec3 relVel = m_designatedTarget->velocity - m_playerEntry->velocity;
                const float rangeRate = glm::dot(relVel, glm::vec3(los / rngM)); // +opening
                closureKt = -rangeRate * 1.94384f;
            }
            const char* typeName = "TGT";
            if (m_deps.entityRegistry) {
                if (const fl::EntityDef* def = m_deps.entityRegistry->byIndex(m_designatedTarget->typeIndex))
                    typeName = def->name.empty() ? def->id.c_str() : def->name.c_str();
            }
            std::snprintf(m_tgtLabel, sizeof(m_tgtLabel), "TGT %s  %.1f km  %+.0f kt", typeName, rngKm, closureKt);
            HudElement& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
            el = {};
            el.type = HudElement::Type::Text;
            el.x = std::clamp(p->x, 0.05f, 0.95f);
            el.y = std::clamp(p->y + 0.05f, 0.05f, 0.95f);
            el.align = HudAlign::Center;
            el.r = br;
            el.g = bg;
            el.b = bb;
            el.a = 1.0f;
            el.scale = 1.f;
            el.text = m_tgtLabel;
        }
    }

    // Target-slaved inset border (#698): frame the live 3D repeater the renderer draws. Four Lines
    // around the same normalized rect the inset camera renders into.
    if (m_insetActive && m_elementCount + 4 <= kMaxElements) {
        const float x0 = m_insetRect.x, y0 = m_insetRect.y;
        const float x1 = m_insetRect.x + m_insetRect.z, y1 = m_insetRect.y + m_insetRect.w;
        auto border = [&](float ax, float ay, float bx, float by) {
            HudElement& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
            el = {};
            el.type = HudElement::Type::Line;
            el.x = ax;
            el.y = ay;
            el.x2 = bx;
            el.y2 = by;
            el.strokeWidth = 1.5f;
            el.r = 0.f;
            el.g = 1.f;
            el.b = 0.f;
            el.a = 1.f;
        };
        border(x0, y0, x1, y0);
        border(x1, y0, x1, y1);
        border(x1, y1, x0, y1);
        border(x0, y1, x0, y0);
    }

    // Padlock lock-state cue (#697): PADLOCK / PADLOCK — BREAK / REACQ, top-centre under the lubber.
    if (m_deps.camInput && m_deps.cameraController && m_deps.cameraController->mode() == fl::CameraMode::Padlock &&
        m_elementCount < kMaxElements) {
        const char* cue = nullptr;
        switch (m_deps.camInput->padlockState()) {
        case fl::PadlockState::Locked:
            cue = "PADLOCK";
            break;
        case fl::PadlockState::Breaking:
            cue = "PADLOCK -- BREAK";
            break;
        case fl::PadlockState::Reacquire:
            cue = "REACQ";
            break;
        case fl::PadlockState::Off:
            break;
        }
        if (cue) {
            HudElement& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
            el = {};
            el.type = HudElement::Type::Text;
            el.x = 0.5f;
            el.y = 0.13f;
            el.align = HudAlign::Center;
            el.r = 0.9f;
            el.g = 0.9f;
            el.b = 0.2f;
            el.a = 1.0f;
            el.scale = 1.f;
            el.text = cue;
        }
    }

    if (m_deps.wingmanMenu) {
        for (const auto& e : m_deps.wingmanMenu->buildElements()) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }
    if (m_deps.commsMenu) {
        for (const auto& e : m_deps.commsMenu->buildElements()) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }
    if (m_deps.manual && m_deps.manual->isOpen()) {
        for (const auto& e : m_deps.manual->elements()) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }
    // Observer picker label (#860): name + faction of the entity being viewed, top-centre.
    if (m_pickerLabel[0] && m_elementCount < kMaxElements) {
        HudElement& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = {};
        el.type = HudElement::Type::Text;
        el.x = 0.5f;
        el.y = 0.04f;
        el.align = HudAlign::Center;
        el.r = 0.6f;
        el.g = 0.9f;
        el.b = 1.0f;
        el.a = 1.0f;
        el.scale = 1.f;
        el.text = m_pickerLabel;
    }
    // Crew seat picker overlay (#975): the current selection + a hint, bottom-left. Rebuilt each frame
    // into a member buffer (HudElement::text is non-owning).
    auto addLine = [&](const char* txt, float y, float g) {
        if (!txt[0] || m_elementCount >= kMaxElements)
            return;
        HudElement& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = {};
        el.type = HudElement::Type::Text;
        el.x = 0.02f;
        el.y = y;
        el.align = HudAlign::Left;
        el.r = 0.7f;
        el.g = g;
        el.b = 0.5f;
        el.a = 1.0f;
        el.scale = 1.f;
        el.text = txt;
    };
    if (m_seatPickerActive) {
        if (const fl::SeatTarget* t = m_seatPicker.selected())
            std::snprintf(m_seatPickerLine, sizeof(m_seatPickerLine), "SEAT: %s @%u  [K next  L join  U leave]",
                          t->role.c_str(), t->entityIdx);
        else
            std::snprintf(m_seatPickerLine, sizeof(m_seatPickerLine), "SEAT: no free crew seats");
        addLine(m_seatPickerLine, 0.86f, 0.9f);
    }
    addLine(m_seatResultLine, 0.90f, 0.9f);

    // Replay transport bar (#41), appended last so it draws over the HUD rather than under it.
    if (m_deps.replay && m_deps.replay->isOpen()) {
        static const fl::PhotoModeState kNoPhoto{};
        for (const auto& e : m_replayHud.build(*m_deps.replay, m_deps.photo ? *m_deps.photo : kNoPhoto)) {
            if (m_elementCount >= kMaxElements)
                break;
            m_elements[static_cast<std::size_t>(m_elementCount++)] = e;
        }
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}

} // namespace fl