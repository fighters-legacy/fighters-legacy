// SPDX-License-Identifier: GPL-3.0-or-later
#include "JoystickDevices.h"
#include "IJoystick.h"
#include <algorithm>

namespace fl {

void JoystickDevices::update(const IJoystick& js) {
    m_changes.clear();

    std::vector<Device> live;
    const int count = js.getJoystickCount();
    live.reserve(static_cast<size_t>(count > 0 ? count : 0));
    for (int i = 0; i < count; ++i) {
        const char* guid = js.getJoystickGuid(i);
        const char* name = js.getJoystickName(i);
        // A device with no GUID cannot be addressed persistently; it is still usable through an `Any`
        // binding, so it stays in the list with an empty guid rather than being dropped.
        live.push_back({guid ? guid : "", name ? name : "", i});
    }

    for (const auto& d : live) {
        const bool known =
            std::any_of(m_devices.begin(), m_devices.end(), [&](const Device& o) { return o.guid == d.guid; });
        if (!known)
            m_changes.push_back({d.guid, d.name, true});
    }
    for (const auto& d : m_devices) {
        const bool stillHere = std::any_of(live.begin(), live.end(), [&](const Device& o) { return o.guid == d.guid; });
        if (!stillHere)
            m_changes.push_back({d.guid, d.name, false});
    }

    m_devices = std::move(live);

    // Hat state: roll current -> previous for devices we already knew, and sample the live positions.
    // Keyed by GUID so a removal in the middle of the list cannot shift one stick's history onto
    // another's and manufacture an edge.
    std::vector<HatState> hats;
    hats.reserve(m_devices.size());
    for (const auto& d : m_devices) {
        HatState st;
        st.guid = d.guid;
        const auto it = std::find_if(m_hats.begin(), m_hats.end(), [&](const HatState& h) { return h.guid == d.guid; });
        if (it != m_hats.end())
            st.previous = it->current;
        const int hatCount = js.getHatCount(d.index);
        st.current.resize(static_cast<size_t>(hatCount > 0 ? hatCount : 0), HatPosition::Centered);
        st.previous.resize(st.current.size(), HatPosition::Centered);
        for (int h = 0; h < hatCount; ++h)
            st.current[static_cast<size_t>(h)] = js.getHatPosition(d.index, h);
        hats.push_back(std::move(st));
    }
    m_hats = std::move(hats);
}

int JoystickDevices::resolve(const DeviceRef& ref) const {
    if (m_devices.empty())
        return kAbsent;
    // `Any` means "whichever stick is plugged in": the first one. That is the pre-#1061 hardcoded
    // device 0, promoted from an assumption to a stated rule.
    if (ref.isAny())
        return m_devices.front().index;
    for (const auto& d : m_devices)
        if (d.guid == ref.guid)
            return d.index;
    return kAbsent;
}

const JoystickDevices::HatState* JoystickDevices::hatStateFor(int deviceIndex) const {
    for (size_t i = 0; i < m_devices.size(); ++i) {
        if (m_devices[i].index != deviceIndex)
            continue;
        return i < m_hats.size() ? &m_hats[i] : nullptr;
    }
    return nullptr;
}

bool JoystickDevices::hatJustPressed(int deviceIndex, uint32_t hatIndex, HatPosition direction) const {
    const HatState* st = hatStateFor(deviceIndex);
    if (!st || hatIndex >= st->current.size() || hatIndex >= st->previous.size())
        return false;
    const HatPosition now = st->current[hatIndex];
    const HatPosition before = st->previous[hatIndex];
    // An edge is "matches now and did not match then" — not "position changed". Rolling a hat from Up
    // to UpRight must not re-fire a control bound to Up, because the player never released it.
    return hatMatches(direction, now) && !hatMatches(direction, before);
}

} // namespace fl
