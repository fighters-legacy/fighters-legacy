# Installation

Fighters Legacy is in **primary development**. There is no installer yet: releases ship as archives
you unpack and run, and things change between them. The
[changelog](https://github.com/fighters-legacy/fighters-legacy/blob/main/CHANGELOG.md) is the honest account of what moved.

## System requirements

The engine renders with Vulkan 1.3 and will not start without a driver that provides it.

|  | Minimum | Comfortable |
|---|---|---|
| **GPU** | Vulkan 1.3 capable | Anything from the last few generations |
| **CPU** | 4 cores | 8 cores |
| **RAM** | 8 GB | 16 GB |
| **Disk** | ~2 GB for the game | More for terrain and satellite imagery |
| **OS** | Windows 10/11, Linux, macOS 13+ | — |

macOS runs through MoltenVK. Terrain streams from disk as you fly, so the install grows with how
much of the world you have visited.

These are the shapes of the requirement, not measured minimums — a formal table is tracked in
[#58](https://github.com/fighters-legacy/fighters-legacy/issues/58).

## Getting the game

Download the archive for your platform from
[Releases](https://github.com/fighters-legacy/fighters-legacy/releases) and unpack it anywhere you
have write access. There is nothing to install and nothing that touches the system outside that
directory.

The archive contains both the game client and `fl-server`. **Do not delete the server** — the game
starts it in the background for single-player, so removing it breaks offline play.

To build from source instead, see the [Development guide](../developer/development.md).

## Content packs

The engine ships no aircraft, terrain or missions of its own. It is content-agnostic by design:
everything you fly comes from a **content pack**.

You can start the game with no packs at all — there is a compiled-in sandbox with a flyable debug
aircraft, working weapons, procedural terrain and audio, which exists so the whole game is provable
with zero content. It is a test article, not something to enjoy.

For actual aircraft, install **[fl-base-pack](https://github.com/fighters-legacy/fl-base-pack)**:

1. Download the pack.
2. Unpack it into a `mods/` directory beside the game executable, so you end up with
   `mods/fl-base-pack/manifest.toml`.
3. Start the game. Loaded packs are listed in the log at startup.

A pack is just a directory with a `manifest.toml` in it, and packs stack — later ones override
earlier ones. See the [Modding Guide](../modding/index.md) to make your own.

## First run

The game writes its configuration and saves to a per-user directory (not into the install), so an
upgrade is a matter of replacing the game files. Your settings, keybindings and pilot profile
survive it.

Run the executable. You should get a menu with **Instant Action** and **Free Flight**; from there,
the [quick start](quickstart.md) takes you through your first flight.

## If it does not start

**No window, or an immediate exit.** Almost always Vulkan. Confirm your driver provides it —
`vulkaninfo` on Linux, `dxdiag` on Windows. An out-of-date GPU driver is the usual cause.

**No sound on Linux.** Check `~/.config/alsoft.conf` for `drivers=null`, which silences OpenAL
entirely and is not a game bug. Diagnose with `ALSOFT_LOGLEVEL=3` and the bundled `audio_check`
tool.

**A gamepad or HOTAS is not detected on Linux.** See [Linux gamepad setup](gamepad-linux.md) —
most devices need a udev rule before they are readable by a normal user.

**Everything is flat grey and there are no aircraft.** No content pack is mounted; you are in the
zero-content sandbox. Install fl-base-pack as above.

Still stuck? [Open an issue](https://github.com/fighters-legacy/fighters-legacy/issues) with your
platform, GPU and the startup log.
