# Windows validation environment

A Vagrant/libvirt **Windows Server VM** that runs the real MSVC toolchain against your local branch,
so a Windows-only failure surfaces before the push instead of after it (#1114).

**This is optional.** Nothing requires it, and no git hook invokes it — the hosted CI legs remain the
authority. Reach for it when a branch looks MSVC-risky (new translation units, template-heavy code,
CRT calls, anything touching `platform/`); skip it the rest of the time.

**It is a correctness environment, not a measurement one.** It is sized generously off the host
rather than pinned, and it renders through software Vulkan by default, so any timing taken here is
meaningless. [`tools/bot_swarm/reference-env/`](../bot_swarm/reference-env/) — the pinned
8-vCPU/16 GB profile — remains the only instrument a performance number may come from.

## Why it exists

The local pre-push warning sweep uses g++ (plus `-Wshadow` and `-Wfloat-conversion`) as a *proxy*
for MSVC. The proxy has been wrong repeatedly, each time costing a push → CI → fix round trip:

| Failure | What Linux said |
|---|---|
| **C4456** shadowed local (#1037) | clean — `-Wshadow` is in neither `-Wall` nor `-Wextra` |
| **C4244** `int`→`float` narrowing (#1041) | clean — needs `-Wfloat-conversion` |
| **C2039** `<string_view>` does not provide `<string>` (#1007) | clean on GCC *and* Apple Clang |

`debug-msvc` inherits `CMAKE_COMPILE_WARNING_AS_ERROR=ON`, so in this VM all three are build
failures rather than predictions.

## Files

- `Vagrantfile` — the guest definition (libvirt, WinRM, optional GPU passthrough + runner tiers).
- `provision.ps1` — toolchain install: VS Build Tools, Vulkan SDK **+ loader**, Ninja, CMake, git,
  vcpkg, **OpenSSL**, lavapipe, and PowerShell 7 (the shell GitHub's Windows runners use for `run:`
  steps, so the tiers execute under the same interpreter CI uses rather than Windows PowerShell 5.1).
- `run-checks.ps1` — the in-guest driver; runs the tiers below. Also used by the CI runner tier.
- `run-windows-check.sh` — host entry point: bundles the branch, runs the checks, fetches artifacts.
- `setup-runner.ps1` / `runner-cleanup.ps1` — optional self-hosted Windows runner + its cleanup hook.

### ⚠ The compiler major version must match CI

CI builds with **Visual Studio 18 (2026)**, MSVC toolset `14.51.x`. Provisioning installs VS 2026
Build Tools from `aka.ms/vs/18/insiders/vs_buildtools.exe` for exactly that reason, and
`run-checks.ps1` prints the version it is using and **warns loudly if it is older**.

This is not pedantry. With VS 2022 (toolset 14.44) the `ci` tier fails to compile
`tests/test_http_admin.cpp` with `C2017: illegal escape sequence` — a raw string containing `\"`
inside a `CHECK()` macro, which 14.44's traditional preprocessor mis-tokenizes and 14.51 accepts.
Nothing is wrong with that code and CI compiles it happily. **A validation tool that invents
failures is worse than no validation tool**, so if you see the version warning, re-provision before
believing anything the tiers tell you.

Two Microsoft-specific traps worth knowing if you touch this:

- `vswhere` **hides prerelease instances unless you pass `-prerelease`**. The VS 2026 Build Tools
  install as an "Insiders" instance, so every query here passes that flag — without it the scripts
  silently report the older VS 2022 that is still on the box.
- `aka.ms` answers an **unknown** alias with a `200 OK` Bing search page rather than a 404. Only
  `vs/17/release`, `vs/17/pre`, `vs/18/insiders` and `vs/insiders` resolve to a real installer.
  `Get-RemoteFile` validates magic bytes on arrival *and* keys its cache on the URL, because a
  cached-but-wrong installer is a valid PE that quietly installs the wrong compiler.

## Quick start

    # one-time (long: VS Build Tools alone is 30-60 minutes)
    cd tools/windows-env && vagrant up --provider=libvirt

    # any time after, from anywhere in the repo
    tools/windows-env/run-windows-check.sh

    # just the build + ctest leg, which is what catches the warning classes above
    FL_WINENV_TIERS=ci tools/windows-env/run-windows-check.sh

### Host prerequisites

⚠ **Use HashiCorp's Vagrant, not your distribution's package.** A Windows guest is driven over
WinRM, and Vagrant's WinRM communicator hard-requires the `winrm`, `winrm-fs` and `winrm-elevated`
gems. Fedora's `vagrant` RPM patches in **rubyzip 3.x**, while every published `winrm-fs` (newest:
1.3.5, 2020) caps at `rubyzip ~> 2.0` — there is no version combination that resolves, so
`vagrant up` dies with `cannot load such file -- winrm` before it even fetches the box, and
`vagrant plugin install` cannot fix it. Upstream Vagrant wants `rubyzip ~> 2.3`, so this is a
packaging artifact rather than an upstream limitation. HashiCorp's build bundles its own Ruby with
all three gems already present.

    # Fedora — replaces the distro package (removes vagrant-libvirt with it)
    sudo dnf config-manager addrepo --from-repofile=https://rpm.releases.hashicorp.com/fedora/hashicorp.repo
    sudo dnf install -y --allowerasing --repo=hashicorp vagrant
    sudo dnf install -y libvirt-devel      # to build the plugin against
    vagrant plugin install vagrant-libvirt # no sudo

⚠ **If you also use [`reference-env`](../bot_swarm/reference-env/):** newer `vagrant-libvirt`
defaults to `qemu:///system`. A VM created under `qemu:///session` will read as *not created*, and
running `vagrant status` in that state **deletes** `.vagrant/machines/default/libvirt/id`, detaching
Vagrant from a VM that is otherwise fine. Export `LIBVIRT_DEFAULT_URI=qemu:///session` for that
directory, or restore the file by writing the domain's UUID
(`virsh --connect qemu:///session domuuid <domain>`) back into it. The VM and its disk are never
touched.

## Check tiers

Selected with `FL_WINENV_TIERS` (comma-separated, default `ci,smoke` — `runtime` is opt-in).

| Tier | Mirrors | What runs |
|---|---|---|
| `ci` | the `windows-latest` leg of [`ci.yml`](../../.github/workflows/ci.yml) | `cmake --preset debug-msvc` under the vcpkg toolchain (triplet `x64-windows-static-md`), the `FL_ENABLE_GNS:BOOL=ON` cache assertion, the full build, the `--version` smokes, then `ctest --preset debug-msvc` |
| `smoke` | `windows-smoke` in [`scale-gate.yml`](../../.github/workflows/scale-gate.yml) | `cmake --preset release-msvc -DFL_ENABLE_GNS=OFF`, build `fl-server` + `bot_swarm`, then `run_loadtest.ps1 build\release-msvc 8 3 weave` |
| `runtime` ⚠ | *nothing — no hosted equivalent* | builds the game, renders `builtin:shape-gallery` and asserts a `--screenshot` PNG came back. **Does not currently work — opt in explicitly, see below.** |

⚠ **`runtime` is not in the default tier set and does not pass on a headless guest.** The game gets
as far as `window init failed` (`Game.cpp`) and exits: the emulated display is not enough for SDL to
create a window. This was verified both over WinRM *and* from a scheduled task running in an
autologon console session with `explorer.exe` present — so an earlier revision of this README was
wrong to offer the scheduled-task path as a workaround. It is not one. The plausible routes are the
GPU passthrough profile below (untested for this) or a virtual display driver; neither is verified.
The tier itself is sound — it builds the game and reports the engine log on failure — so it is kept
for when the display question is solved. Run it with `FL_WINENV_TIERS=runtime`.

Getting that far also required the **Vulkan loader** (`vulkan-1.dll` ships with GPU drivers, and
there is no GPU) — a silent failure, since the game linked cleanly and then died at startup with
`0xC0000135`.

The guest also needed an **emulated audio device**, because the game used to treat a failed
`alcOpenDevice` as fatal and exit before opening a window. **Fixed in #1117**: a missing audio device
now degrades to a silent game with a warning, so a headless guest no longer needs a sound card at
all, and `--no-audio` skips the probe outright. The emulated card is kept because it costs nothing
and keeps the guest closer to a real machine.

The GNS assertion in tier `ci` is not decoration: `cmake/dependencies.cmake` *silently* force-disables
GNS when protobuf/OpenSSL are missing, so a broken vcpkg setup would otherwise build enet6-only and
pass. Tiers `smoke` and `runtime` build the lean enet6-only tree, which needs no vcpkg at all.

## How your branch reaches the guest

Not through a synced folder — every option is bad for a Windows guest on libvirt (rsync needs rsync
in the guest, virtiofs needs WinFsp, SMB needs host samba). Instead:

1. The host wrapper bundles `merge-base(origin/main, HEAD)..HEAD` and uploads it over WinRM.
2. The guest keeps a **persistent clone** at `C:\fl\src` (cloned from GitHub on first run, which also
   guarantees the bundle's prerequisite commits) and fast-forwards it to your exact SHA.
3. Build trees at `C:\fl\src\build\{debug,release}-msvc` **persist between runs**, so a repeat check
   is a warm incremental MSVC build. That is the main advantage over waiting for CI.

⚠ **Only committed work is validated.** The bundle is built from commits; the wrapper warns when
your working tree is dirty, but it cannot ship uncommitted changes.

## Tuning knobs

| Var | Default | Meaning |
|---|---|---|
| `FL_WINENV_TIERS` | `ci,smoke` | which tiers to run (`runtime` is opt-in; see above) |
| `FL_WINENV_BOX` | `peru/windows-server-2022-standard-x64-eval` | Vagrant box (any Windows box with a libvirt artifact + WinRM) |
| `FL_WINENV_CPUS` | `12` | guest vCPUs |
| `FL_WINENV_MEM_MB` | `20480` | guest RAM |
| `FL_WINENV_DISK_GB` | `120` | guest disk (VS + SDK + two build trees + vcpkg cache) |
| `FL_WINENV_VULKAN_VER` | `1.3.290.0` | Vulkan SDK version (matches the CI pin) |
| `FL_WINENV_MESA_VER` | `25.0.7` | mesa-dist-win release providing lavapipe |
| `FL_WINENV_VS_BOOTSTRAP` | VS 2026 channel | Build Tools bootstrapper URL (`/17/` = 2022, `/18/` = 2026) |
| `FL_WINENV_GPU` | unset | `1` = pass the host GPU through and render on it |
| `FL_WINENV_NVIDIA_URL` | unset | driver package URL for the GPU profile |
| `FL_WINENV_RUNTIME_VIA_TASK` | unset | `1` = run the runtime tier via a scheduled task (see troubleshooting) |
| `FL_WINENV_RUNNER` | unset | `1` = also register the self-hosted runner during `vagrant up` |

## GPU passthrough profile (optional)

By default the runtime tier renders on **lavapipe** (software Vulkan), which needs no host changes
and is enough to prove the renderer starts and draws. `FL_WINENV_GPU=1` instead hands a real GPU to
the guest, so the tier exercises the production driver path.

This works on a box whose CPU has an integrated GPU: the host keeps its desktop on the iGPU while
the VM owns the discrete card. libvirt attaches the device with `managed="yes"`, so it is detached
from the host driver when the VM starts and **reattached when the VM stops** — no static `vfio-pci`
bind, and the host gets the card back for CUDA/inference the moment you `vagrant halt`.

**One-time host setup — do this by hand, it is deliberately not automated** (it touches firmware and
a hand-maintained boot configuration):

1. Enable **VT-d** in firmware. Verify afterwards: `ls /sys/class/iommu/` must not be empty.
2. Append `intel_iommu=on iommu=pt` to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`, then
   `sudo grub2-mkconfig -o /boot/grub2/grub.cfg`. **Additive edit only** — leave everything else in
   the boot configuration alone.
3. Move the host display to the iGPU (motherboard video output).
4. Check the IOMMU grouping before going further — the card must not share a group with unrelated
   devices:

       for g in /sys/kernel/iommu_groups/*/devices/*; do
           echo "group $(basename "$(dirname "$(dirname "$g")")"): $(lspci -nns "$(basename "$g")")"
       done | sort -V | grep -i nvidia

   If the GPU's group contains anything besides its own video and audio functions, stop. An ACS
   override patch would "fix" it by lying to the kernel about isolation; not worth it for a
   convenience feature whose fallback (lavapipe) already works.
5. Point `FL_WINENV_NVIDIA_URL` at a driver package and `vagrant provision`, or install the driver
   in the guest by hand.

Then: `FL_WINENV_GPU=1 vagrant up --provider=libvirt`, and run checks with `FL_WINENV_GPU=1` so the
runtime tier leaves the Vulkan loader alone instead of forcing lavapipe.

**Costs, plainly:** while the VM is running the host has no discrete GPU, so local LLM inference
pauses. Nothing on the host may be holding the card when the VM starts, or the detach fails with
a device-busy error. And a passed-through GPU with no monitor attached may refuse to present a
swapchain — if that happens, an HDMI dummy plug on the card is the usual fix, or fall back to
`FL_WINENV_RUNTIME_VIA_TASK=1`. Set `FL_WINENV_GPU_BUS`/`FL_WINENV_GPU_SLOT` if your card is not at
`01:00`.

## Self-hosted Windows runner (optional)

The same tiers can be dispatched at this VM from GitHub Actions via
[`windows-env.yml`](../../.github/workflows/windows-env.yml). This is a convenience, not a gate:
the required Windows checks already run on hosted runners for every PR.

### Register

1. Get a short-lived **registration token**: repo → Settings → Actions → Runners → *New self-hosted
   runner*. It expires in about an hour and is never committed.
2. Bring the VM up with the runner provisioner on:

       FL_WINENV_RUNNER=1 FL_RUNNER_TOKEN=<token> vagrant up --provider=libvirt

   Or, if the VM already exists, re-run provisioning with the same variables (`provision.ps1` is
   idempotent, so this is quick):

       FL_WINENV_RUNNER=1 FL_RUNNER_TOKEN=<token> vagrant provision

3. Verify: the runner shows **Idle** in repo Settings → Runners with labels
   `self-hosted, windows, x64, fl-windows`.
4. Flip the CI on-switch: set repo variable `FL_WINDOWS_RUNNER_READY = true` (Settings → Actions →
   Variables). The job stays skipped until this is set.

### Run

    gh workflow run windows-env.yml -f tiers=ci,smoke

Runs are **manual** (`workflow_dispatch`) because this machine is not always on — GitHub does not
queue cron runs missed while a runner is offline, so a schedule would silently skip rather than
report. The cron line is preserved commented in the workflow for a one-line re-enable.

The default is `ci,smoke`. Add `runtime` only after it has been verified under the runner's service
account, which is the session-0 case described below.

### Security

A self-hosted runner on a public repo must never execute fork-PR code. The layers:

- **Trigger guard** — the job is `workflow_dispatch`-only (write-access-gated) behind
  `github.repository == 'fighters-legacy/fighters-legacy'` and `vars.FL_WINDOWS_RUNNER_READY`.
  No `pull_request` path reaches it; the PR tier stays on hosted runners.
- **The VM is the sandbox** — a compromise's blast radius is a disposable guest. There is no host
  filesystem mount: source arrives by upload or checkout, never through a synced folder.
- **Dedicated non-administrator service account** (`flrunner`). Windows has no equivalent of the
  systemd hardening drop-in the Linux reference runner installs, so the unprivileged account, the VM
  boundary and the trigger guard are the compensating controls.
- Set repo → Settings → Actions → *Fork pull request workflows* → **require approval for all
  outside collaborators**.

### Unregister

Set `FL_WINDOWS_RUNNER_READY = false` (or delete it) to disable the CI job immediately. To remove
the runner itself: `cd C:\actions-runner; .\config.cmd remove --token <removal-token>`, or
`vagrant destroy` to tear down the whole VM.

## Troubleshooting

**The runtime tier produces no screenshot.** Expected — see the tier table above. The game exits at
`window init failed`; tiers `ci` and `smoke` are unaffected, being headless by construction (they
are what the hosted runners do today). `FL_WINENV_RUNTIME_VIA_TASK=1` runs the game from a scheduled
task in the console session and is kept for further experiments, but **it was tested with autologon
enabled and `explorer.exe` running, and the window still failed to initialise** — do not expect it
to help as-is:

    # in the guest, elevated — replace the password with the box's vagrant account password
    $k = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
    Set-ItemProperty $k AutoAdminLogon 1; Set-ItemProperty $k DefaultUserName vagrant
    Set-ItemProperty $k DefaultPassword vagrant

    # from the host, after a reboot
    FL_WINENV_RUNTIME_VIA_TASK=1 FL_WINENV_TIERS=runtime tools/windows-env/run-windows-check.sh

When a run does fail, the tier prints the game's stdout, stderr **and the newest engine log** from
`%APPDATA%\mkzsystems\fighters-legacy\logs`. That last one matters: the game logs to a file rather
than to a console, so both pipes come back empty and the failure looks mute without it.

**The evaluation licence expired.** The guest is a 180-day Microsoft evaluation image. `slmgr /rearm`
buys another period; otherwise `vagrant destroy -f && vagrant up` rebuilds from the box. A rebuild
re-provisions everything and orphans any runner registration, so re-register afterwards.

**`vagrant up` cannot find the box.** Box hosting rots — the Linux reference VM had to settle a
release behind for exactly this reason. Point `FL_WINENV_BOX` at any Windows box that publishes a
libvirt artifact and speaks WinRM.

**The Build Tools installer asked for a reboot.** Exit code 3010 is a success with a pending reboot;
`vagrant reload` before the first check run.

**A WinRM step times out mid-install.** The Vagrantfile already raises `winrm.timeout`; if a
particular installer still overruns it, run that step directly with
`vagrant winrm -c "..."` (or `vagrant powershell -c "..."`) and then re-run `vagrant provision`,
which skips whatever already succeeded.

**Everything rebuilds every run.** Check that the guest is reusing `C:\fl\src` rather than a fresh
checkout, and that `C:\fl` is still on the Defender exclusion list (`Get-MpPreference`).

**The `ci` tier says GNS was force-disabled.** GNS needs OpenSSL, which is **not** in the repo's
`vcpkg.json` — CI never had to ask for it because GitHub's runner image ships OpenSSL preinstalled.
Provisioning installs it and sets `OPENSSL_ROOT_DIR`. If the tier still complains after that, note
that **CMake never retries a cached `NOTFOUND`**: an earlier configure recorded
`OPENSSL_INCLUDE_DIR-NOTFOUND` and every later configure reuses it, so installing the dependency
appears to have no effect. The tier purges the configure cache and retries once before believing
the assertion; `rm -r build/debug-msvc` in the guest is the manual equivalent.

**The `runtime` tier exits with `-1073741515` (`0xC0000135`).** That is `STATUS_DLL_NOT_FOUND`, and
it names no file. The usual cause is a missing **Vulkan loader**: `vulkan-1.dll` normally arrives
with a GPU driver, and a headless guest has none — the SDK ships headers and tools but not the
loader, and mesa ships the ICD and driver but not the loader either. Provisioning installs LunarG's
Vulkan runtime for this. The tier now captures the game's stdout/stderr to
`C:\fl\out\runtime-smoke.{out,err}.txt` and echoes them on failure, so later failures explain
themselves rather than reporting a bare exit code.

## Deferred

- **Any Windows performance measurement.** Even with a passed-through GPU this is a VM with unpinned
  CPU scheduling. `reference-env` stays the only measurement instrument.
- **Cron scheduling of the runner job** — GitHub does not queue missed crons on an intermittently-on
  runner.
- **Windows containers** — impossible on a Linux kernel; this is a VM by construction.
- **Interactive `visual_check.ps1` in the guest** — needs a real desktop (attach with `virt-viewer`
  if you want it); the runtime tier's `--screenshot` covers the automated need.
- **Looking Glass / desktop streaming**, box and licence refresh automation, MSVC channel pinning,
  ARM64 Windows.
