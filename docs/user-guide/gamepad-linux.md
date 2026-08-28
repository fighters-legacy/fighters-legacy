# Linux Gamepad Setup

On Windows and macOS, Xbox controllers work over Bluetooth without any extra setup. On Linux, the default `hid_microsoft` kernel driver connects the controller but does not send the Xbox BT protocol handshake that tells the hardware to start reporting game input. The result is a controller that appears connected but produces no button or axis events.

The fix is the `xpadneo` DKMS kernel module.

---

## Symptom

The game (or `input_test`) detects a gamepad and shows it as connected, but pressing buttons and moving sticks has no effect.

---

## Install xpadneo

`xpadneo` is a DKMS module — it rebuilds automatically when your kernel updates.

### Fedora (primary maintainer platform)

```bash
sudo dnf install -y dkms kernel-devel kernel-headers
git clone https://github.com/atar-axis/xpadneo.git
cd xpadneo
sudo ./install.sh
```

### Ubuntu / Debian

```bash
sudo apt-get install -y dkms linux-headers-$(uname -r)
git clone https://github.com/atar-axis/xpadneo.git
cd xpadneo
sudo ./install.sh
```

After install, reconnect the controller (disconnect from Bluetooth and pair again, or restart the Bluetooth service). A reboot is sometimes needed to fully unload `hid_microsoft`.

---

## udev rules (hidraw permissions)

By default the hidraw device node (`/dev/hidrawX`) is root-only. Create a udev rule so your user can access it:

```bash
sudo tee /etc/udev/rules.d/60-xbox-bt-gamepad.rules > /dev/null <<'EOF'
SUBSYSTEM=="input", ATTRS{name}=="Xbox Wireless Controller", ENV{ID_INPUT_JOYSTICK}="1"
KERNEL=="hidraw*", KERNELS=="0005:045E:0B13.*", SUBSYSTEM=="hidraw", MODE="0660", GROUP="input"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then add your user to the `input` group and re-login (or use `newgrp input` in the current session):

```bash
sudo usermod -aG input $USER
```

---

## Verify

```bash
# Should show EV_KEY and EV_ABS events when you press buttons / move sticks
sudo evtest /dev/input/event$(ls -t /dev/input/event* | head -1 | grep -o '[0-9]*$')
```

Or run the included test tool:

```bash
./build/debug/tools/input_test
```

All buttons, axes, and rumble should respond correctly.

---

## Notes

- This setup has been tested with the **Microsoft Xbox Elite Series 2** over Bluetooth on Fedora fc44.
- Wired USB connections typically work without xpadneo.
- SDL3 is configured in the engine to deliver joystick and gamepad events regardless of window focus (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`), so the game does not need to be the focused window for controller input to work.

---

## HOTAS and joystick devices

HOTAS throttle quadrants, rudder pedals, POV hats and raw flight sticks are supported via the
`IJoystick` API. Every axis, button and hat direction is an ordinary binding in
`config/bindings.toml`, tuned per device — see [Controls](controls.md#hotas-controls). Two sticks are
addressed independently, by GUID rather than by device order, so their bindings survive a replug.

HOTAS throttle quadrants, rudder pedals, and flight sticks appear as evdev nodes
(`/dev/input/eventX`) on Linux. Adding your user to the `input` group (see above) covers
most devices. If your HOTAS is not responding, create a udev rule for its vendor name:

```bash
sudo tee /etc/udev/rules.d/61-hotas.rules > /dev/null <<'EOF'
KERNEL=="event*", SUBSYSTEM=="input", ATTRS{name}=="*Thrustmaster*", MODE="0664", GROUP="input"
KERNEL=="event*", SUBSYSTEM=="input", ATTRS{name}=="*CH Products*",   MODE="0664", GROUP="input"
KERNEL=="event*", SUBSYSTEM=="input", ATTRS{name}=="*Logitech*",      MODE="0664", GROUP="input"
KERNEL=="event*", SUBSYSTEM=="input", ATTRS{name}=="*VIRPIL*",        MODE="0664", GROUP="input"
KERNEL=="event*", SUBSYSTEM=="input", ATTRS{name}=="*VKB*",           MODE="0664", GROUP="input"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

To find the exact name for your device, run `udevadm info /dev/input/eventX` (where `X`
is the event number shown in `evtest`) and match on `ATTRS{name}`. Add a new line for
your brand using the same pattern.

Reconnect the device after applying the rules (or run `sudo udevadm trigger`).

### Axis config

Axis assignments, deadzone, response curve, inversion and mode are all in
`config/bindings.toml` — `[bindings]` for the assignment and `[[axis_config]]` for the tuning. The
`[controls] hotas_*` keys this section used to document were retired in
[#1061](https://github.com/fighters-legacy/fighters-legacy/issues/1061); they are read once on upgrade
and folded into `bindings.toml` automatically. See
[Controls → `config/bindings.toml`](controls.md#configbindingstoml) for the schema.

Default axis layout, applied to **every** connected stick — whichever one you move is in command:

| Axis index | Mapping | Action |
|---|---|---|
| 0 | Aileron (roll) | `RollAxis` |
| 1 | Elevator (pitch) | `PitchAxis` |
| 2 | Throttle (absolute lever) | `ThrottleAxis` |
| 3 | Rudder (yaw) | `YawAxis` |

To pin a binding to one specific device, copy its GUID out of the `[[devices]]` table that
`bindings.toml` records the first time the device is connected (the game also logs the GUID and name of
every device as it arrives).
