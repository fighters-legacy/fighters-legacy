// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "SDL3Joystick.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>

namespace fl {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

int SDL3Joystick::findJoystick(SDL_JoystickID id) const {
    for (int i = 0; i < static_cast<int>(m_joysticks.size()); ++i) {
        if (m_joysticks[i].sdlId == id)
            return i;
    }
    return -1;
}

SDL3Joystick::JoystickState* SDL3Joystick::joystickAt(int joystickId) {
    if (joystickId < 0 || joystickId >= static_cast<int>(m_joysticks.size()))
        return nullptr;
    return &m_joysticks[static_cast<size_t>(joystickId)];
}

const SDL3Joystick::JoystickState* SDL3Joystick::joystickAt(int joystickId) const {
    if (joystickId < 0 || joystickId >= static_cast<int>(m_joysticks.size()))
        return nullptr;
    return &m_joysticks[static_cast<size_t>(joystickId)];
}

static HatPosition hatFromSDL(Uint8 value) {
    switch (value) {
    case SDL_HAT_UP:
        return HatPosition::Up;
    case SDL_HAT_RIGHTUP:
        return HatPosition::UpRight;
    case SDL_HAT_RIGHT:
        return HatPosition::Right;
    case SDL_HAT_RIGHTDOWN:
        return HatPosition::DownRight;
    case SDL_HAT_DOWN:
        return HatPosition::Down;
    case SDL_HAT_LEFTDOWN:
        return HatPosition::DownLeft;
    case SDL_HAT_LEFT:
        return HatPosition::Left;
    case SDL_HAT_LEFTUP:
        return HatPosition::UpLeft;
    default:
        return HatPosition::Centered;
    }
}

// ---------------------------------------------------------------------------
// ISDL3EventSink
// ---------------------------------------------------------------------------

void SDL3Joystick::onSDLEvent(const SDL_Event& ev) {
    switch (ev.type) {

    case SDL_EVENT_JOYSTICK_ADDED: {
        // Skip devices that SDL3 recognises as standard gamepads — SDL3Input owns those.
        if (SDL_IsGamepad(ev.jdevice.which))
            break;
        SDL_Joystick* handle = SDL_OpenJoystick(ev.jdevice.which);
        if (!handle) {
            m_lastError = SDL_GetError();
            break;
        }
        JoystickState js;
        js.sdlId = ev.jdevice.which;
        js.handle = handle;

        const char* name = SDL_GetJoystickName(handle);
        js.name = name ? name : "";

        char guidBuf[33]{};
        SDL_GUIDToString(SDL_GetJoystickGUID(handle), guidBuf, static_cast<int>(sizeof(guidBuf)));
        js.guid = guidBuf;

        int numAxes = SDL_GetNumJoystickAxes(handle);
        int numHats = SDL_GetNumJoystickHats(handle);
        int numButtons = SDL_GetNumJoystickButtons(handle);

        js.axes.assign(static_cast<size_t>(numAxes > 0 ? numAxes : 0), 0.0f);
        js.hats.assign(static_cast<size_t>(numHats > 0 ? numHats : 0), HatPosition::Centered);
        js.buttons.assign(static_cast<size_t>(numButtons > 0 ? numButtons : 0), false);
        js.justPressed.assign(static_cast<size_t>(numButtons > 0 ? numButtons : 0), false);

        // Force feedback (#928): open the haptic device if the stick supports it (a plain gamepad or a
        // non-FFB stick simply reports no haptic — the effects then no-op).
        if (SDL_IsJoystickHaptic(handle))
            js.haptic = SDL_OpenHapticFromJoystick(handle);

        m_joysticks.push_back(std::move(js));

        if (m_eventHandler) {
            int newId = static_cast<int>(m_joysticks.size()) - 1;
            m_eventHandler->onJoystickAdded(newId);
        }
        break;
    }

    case SDL_EVENT_JOYSTICK_REMOVED: {
        int idx = findJoystick(ev.jdevice.which);
        if (idx < 0)
            break;
        if (m_eventHandler)
            m_eventHandler->onJoystickRemoved(idx);
        JoystickState& dead = m_joysticks[static_cast<size_t>(idx)];
        if (dead.haptic) {
            for (int s = 0; s < IJoystick::kFfbSlotCount; ++s)
                if (dead.ffbEffectId[s] >= 0)
                    SDL_DestroyHapticEffect(dead.haptic, dead.ffbEffectId[s]);
            SDL_CloseHaptic(dead.haptic);
        }
        SDL_CloseJoystick(dead.handle);
        m_joysticks.erase(m_joysticks.begin() + idx);
        break;
    }

    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        int idx = findJoystick(ev.jaxis.which);
        if (idx < 0)
            break;
        JoystickState& js = m_joysticks[static_cast<size_t>(idx)];
        int axis = static_cast<int>(ev.jaxis.axis);
        if (axis < 0 || axis >= static_cast<int>(js.axes.size()))
            break;
        // Sint16 range [-32768, 32767]; normalize to [-1.0, 1.0].
        float value = static_cast<float>(ev.jaxis.value) / 32767.0f;
        value = std::max(-1.0f, std::min(1.0f, value));
        js.axes[static_cast<size_t>(axis)] = value;
        break;
    }

    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        int idx = findJoystick(ev.jhat.which);
        if (idx < 0)
            break;
        JoystickState& js = m_joysticks[static_cast<size_t>(idx)];
        int hat = static_cast<int>(ev.jhat.hat);
        if (hat < 0 || hat >= static_cast<int>(js.hats.size()))
            break;
        js.hats[static_cast<size_t>(hat)] = hatFromSDL(ev.jhat.value);
        break;
    }

    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP: {
        int idx = findJoystick(ev.jbutton.which);
        if (idx < 0)
            break;
        JoystickState& js = m_joysticks[static_cast<size_t>(idx)];
        int btn = static_cast<int>(ev.jbutton.button);
        if (btn < 0 || btn >= static_cast<int>(js.buttons.size()))
            break;
        bool pressed = (ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
        js.buttons[static_cast<size_t>(btn)] = pressed;
        if (pressed)
            js.justPressed[static_cast<size_t>(btn)] = true;
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// IJoystick — device enumeration
// ---------------------------------------------------------------------------

int SDL3Joystick::getJoystickCount() const {
    return static_cast<int>(m_joysticks.size());
}

const char* SDL3Joystick::getJoystickName(int joystickId) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js)
        return nullptr;
    return js->name.c_str();
}

const char* SDL3Joystick::getJoystickGuid(int joystickId) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js)
        return nullptr;
    return js->guid.c_str();
}

// ---------------------------------------------------------------------------
// IJoystick — axes
// ---------------------------------------------------------------------------

int SDL3Joystick::getAxisCount(int joystickId) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js)
        return 0;
    return static_cast<int>(js->axes.size());
}

float SDL3Joystick::getAxisValue(int joystickId, int axisIndex) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js || axisIndex < 0 || axisIndex >= static_cast<int>(js->axes.size()))
        return 0.0f;
    return js->axes[static_cast<size_t>(axisIndex)];
}

// ---------------------------------------------------------------------------
// IJoystick — hats
// ---------------------------------------------------------------------------

int SDL3Joystick::getHatCount(int joystickId) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js)
        return 0;
    return static_cast<int>(js->hats.size());
}

HatPosition SDL3Joystick::getHatPosition(int joystickId, int hatIndex) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js || hatIndex < 0 || hatIndex >= static_cast<int>(js->hats.size()))
        return HatPosition::Centered;
    return js->hats[static_cast<size_t>(hatIndex)];
}

// ---------------------------------------------------------------------------
// IJoystick — buttons
// ---------------------------------------------------------------------------

int SDL3Joystick::getButtonCount(int joystickId) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js)
        return 0;
    return static_cast<int>(js->buttons.size());
}

bool SDL3Joystick::isButtonDown(int joystickId, int buttonIndex) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js || buttonIndex < 0 || buttonIndex >= static_cast<int>(js->buttons.size()))
        return false;
    return js->buttons[static_cast<size_t>(buttonIndex)];
}

bool SDL3Joystick::isButtonJustPressed(int joystickId, int buttonIndex) const {
    const JoystickState* js = joystickAt(joystickId);
    if (!js || buttonIndex < 0 || buttonIndex >= static_cast<int>(js->justPressed.size()))
        return false;
    return js->justPressed[static_cast<size_t>(buttonIndex)];
}

// ---------------------------------------------------------------------------
// IJoystick — hot-plug
// ---------------------------------------------------------------------------

void SDL3Joystick::setEventHandler(IJoystickEventHandler* handler) {
    m_eventHandler = handler;
}

// ---------------------------------------------------------------------------
// IJoystick — frame boundary
// ---------------------------------------------------------------------------

void SDL3Joystick::flush() {
    for (auto& js : m_joysticks) {
        std::fill(js.justPressed.begin(), js.justPressed.end(), false);
    }
}

// ---------------------------------------------------------------------------
// IJoystick — error
// ---------------------------------------------------------------------------

const char* SDL3Joystick::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

// ---------------------------------------------------------------------------
// IJoystick — force feedback (#928)
// ---------------------------------------------------------------------------

bool SDL3Joystick::supportsForceFeedback(int joystickId) const {
    const JoystickState* js = joystickAt(joystickId);
    return js && js->haptic != nullptr;
}

bool SDL3Joystick::playFfbEffect(int joystickId, int slot, const FfbEffect& effect) {
    JoystickState* js = joystickAt(joystickId);
    if (!js || !js->haptic || slot < 0 || slot >= IJoystick::kFfbSlotCount)
        return false;

    // Zero-init the union carefully, then fill the active member.
    SDL_HapticEffect eff;
    SDL_memset(&eff, 0, sizeof(eff));
    const Sint16 level = static_cast<Sint16>(std::clamp(effect.magnitude, 0.0f, 1.0f) * 32767.0f);
    const Uint32 length = effect.durationMs == 0 ? SDL_HAPTIC_INFINITY : effect.durationMs;
    if (effect.kind == FfbEffectKind::ConstantForce) {
        eff.type = SDL_HAPTIC_CONSTANT;
        eff.constant.direction.type = SDL_HAPTIC_POLAR;
        eff.constant.direction.dir[0] = static_cast<Sint32>(effect.directionDeg * 100.0f);
        eff.constant.level = level;
        eff.constant.length = length;
    } else {
        eff.type = SDL_HAPTIC_SINE;
        eff.periodic.direction.type = SDL_HAPTIC_POLAR;
        eff.periodic.direction.dir[0] = static_cast<Sint32>(effect.directionDeg * 100.0f);
        eff.periodic.period = static_cast<Uint16>(effect.periodMs);
        eff.periodic.magnitude = level;
        eff.periodic.length = length;
    }
    if (!SDL_HapticEffectSupported(js->haptic, &eff))
        return false;

    // Same kind on this slot -> update in place (smooth amplitude tracking); else replace.
    if (js->ffbEffectId[slot] >= 0 && js->ffbSlotKind[slot] == effect.kind) {
        SDL_UpdateHapticEffect(js->haptic, js->ffbEffectId[slot], &eff);
    } else {
        if (js->ffbEffectId[slot] >= 0)
            SDL_DestroyHapticEffect(js->haptic, js->ffbEffectId[slot]);
        js->ffbEffectId[slot] = SDL_CreateHapticEffect(js->haptic, &eff);
        js->ffbSlotKind[slot] = effect.kind;
        if (js->ffbEffectId[slot] < 0)
            return false;
    }
    return SDL_RunHapticEffect(js->haptic, js->ffbEffectId[slot], 1);
}

void SDL3Joystick::stopFfbEffect(int joystickId, int slot) {
    JoystickState* js = joystickAt(joystickId);
    if (js && js->haptic && slot >= 0 && slot < IJoystick::kFfbSlotCount && js->ffbEffectId[slot] >= 0)
        SDL_StopHapticEffect(js->haptic, js->ffbEffectId[slot]);
}

void SDL3Joystick::stopAllFfbEffects(int joystickId) {
    JoystickState* js = joystickAt(joystickId);
    if (js && js->haptic)
        SDL_StopHapticEffects(js->haptic);
}

} // namespace fl
