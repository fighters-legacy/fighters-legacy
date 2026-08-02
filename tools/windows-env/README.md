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
- `provision.ps1` — toolchain install: VS Build Tools, Vulkan SDK, Ninja, CMake, git, vcpkg, lavapipe,
  and PowerShell 7 (the shell GitHub's Windows runners use for `run:` steps, so the tiers execute
  under the same interpreter CI uses rather than Windows PowerShell 5.1).
- `run-checks.ps1` — the in-guest driver; runs the tiers below. Also used by the CI runner tier.
- `run-windows-check.sh` — host entry point: bundles the branch, runs the checks, fetches artifacts.
- `setup-runner.ps1` / `runner-cleanup.ps1` — optional self-hosted Windows runner + its cleanup hook.

## Quick start

    # one-time (long: VS Build Tools alone is 30-60 minutes)
    cd tools/windows-env && vagrant up --provider=libvirt

    # any time after, from anywhere in the repo
    tools/windows-env/run-windows-check.sh

    # just the build + ctest leg, which is what catches the warning classes above
    FL_WINENV_TIERS=ci tools/windows-env/run-windows-check.sh

Host prerequisites: `vagrant`, the `vagrant-libvirt` plugin, and a working libvirt/KVM stack
(`vagrant plugin install vagrant-libvirt`). The WinRM gems `vagrant upload` needs ship with Vagrant.

## Check tiers

Selected with `FL_WINENV_TIERS` (comma-separated, default `ci,smoke,runtime`).

| Tier | Mirrors | What runs |
|---|---|---|
| `ci` | the `windows-latest` leg of [`ci.yml`](../../.github/workflows/ci.yml) | `cmake --preset debug-msvc` under the vcpkg toolchain (triplet `x64-windows-static-md`), the `FL_ENABLE_GNS:BOOL=ON` cache assertion, the full build, the `--version` smokes, then `ctest --preset debug-msvc` |
| `smoke` | `windows-smoke` in [`scale-gate.yml`](../../.github/workflows/scale-gate.yml) | `cmake --preset release-msvc -DFL_ENABLE_GNS=OFF`, build `fl-server` + `bot_swarm`, then `run_loadtest.ps1 build\release-msvc 8 3 weave` |
| `runtime` | *nothing — no hosted equivalent* | builds the game, renders `builtin:shape-gallery` and asserts a `--screenshot` PNG came back: window + swapchain creation, shader loading, the content path and the embedded `fl-server` handshake, all in one |

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
| `FL_WINENV_TIERS` | `ci,smoke,runtime` | which tiers to run |
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

**The runtime tier produces no screenshot.** A WinRM shell has no interactive desktop, and a Vulkan
swapchain needs a window. Tiers `ci` and `smoke` are unaffected (they are headless by construction —
they run on hosted runners today). For the runtime tier, enable autologon in the guest and re-run
with the scheduled-task path, which executes in the console session instead:

    # in the guest, elevated — replace the password with the box's vagrant account password
    $k = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
    Set-ItemProperty $k AutoAdminLogon 1; Set-ItemProperty $k DefaultUserName vagrant
    Set-ItemProperty $k DefaultPassword vagrant

    # from the host, after a reboot
    FL_WINENV_RUNTIME_VIA_TASK=1 FL_WINENV_TIERS=runtime tools/windows-env/run-windows-check.sh

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
