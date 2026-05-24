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
- SDL3 is configured in the engine to deliver gamepad events regardless of window focus (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`), so the game does not need to be the focused window for controller input to work.
