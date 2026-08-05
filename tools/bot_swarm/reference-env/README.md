# bot_swarm reference environment (8 cores / 16 GB)

Reproducible, constrained environments for characterising `bot_swarm` load on a fixed
**8‑core / 16 GB** profile — the reference instance for the 128+ scale gate (#505, #520). Ad‑hoc
runs on a dev box aren't comparable across machines; pin them to this profile instead.

Two paths, both build **Release** (the dev‑box numbers in #505 were Debug `-O0` and are
pessimistic — always characterise optimized):

| Path | Fidelity | Speed | Use when |
|---|---|---|---|
| **Container** (`run-container.*`) | CPU + RAM constrained via cgroups; shares the host kernel | seconds to start | quick iteration, CI‑like sweeps |
| **VM** (`Vagrantfile`) | own kernel + dedicated vCPUs — closest to a cloud instance | minutes to boot | faithful one‑off benchmarks |

Both run the same in‑guest script (`run-benchmark.sh`): build `fl-server` + `bot_swarm`
Release, then sweep `run_loadtest.sh` over client counts × patterns. Reports land in
`tools/bot_swarm/results/` (git‑ignored). See [docs/developer/load-testing.md](../../../docs/developer/load-testing.md).

**Determinism.** Each path pins a fixed userspace so the toolchain (GCC) doesn't drift with whatever
each distro ships — the **container** on **Fedora 44** (`fedora:44`, GCC 16, the primary benchmark
path) and the **VM** on **Fedora 42** (`alvistack/fedora-42`, GCC 15 — one release behind because no
working 43/44 libvirt box exists). The harness builds only the headless `fl-server` (SDL‑free,
`platform-stdfs` over `std::filesystem`) and `bot_swarm` (a pure enet6 client) with the GPU renderer
disabled, so **neither target compiles SDL3** — the toolchain image carries just the
compiler/cmake/ninja/git and needs no SDL3 build dependencies. (CMake still processes the
FetchContent SDL3 for the client/tool targets, but with the renderer disabled `dependencies.cmake`
forces `SDL_UNIX_CONSOLE_BUILD=ON` so SDL3's configure requires no X11/Wayland/desktop dev packages;
it is declared, never compiled here. Before #711/#716 `fl-server` linked SDL3 for filesystem I/O and
the harness did compile it from source — that's no longer the case.) The only remaining
container↔VM difference is shared‑kernel vs own‑kernel.

## Files

- `Containerfile` — Fedora toolchain image (headless; no Vulkan).
- `run-container.sh` — Linux/macOS host wrapper (`--cpus`/`--cpuset-cpus`/`--memory`).
- `run-container.ps1` — Windows host wrapper (Docker Desktop).
- `Vagrantfile` + `vm-provision.sh` — cross‑platform 8‑vCPU/16 GB VM.
- `run-benchmark.sh` — shared in‑guest build + sweep (used by both paths).

## Quick start

    # Container (Linux / macOS)
    tools/bot_swarm/reference-env/run-container.sh
    # Custom sweep:
    CLIENTS="64 128 256 384" DURATION=60 PATTERNS="idle weave aggressive" \
        tools/bot_swarm/reference-env/run-container.sh

    # VM (any OS with Vagrant)
    cd tools/bot_swarm/reference-env && vagrant up
    vagrant ssh -c 'sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh'

## Per‑OS setup

### Linux (this repo's primary dev platform)
- **Container:** `podman` (preferred) or `docker`; cgroup v2 enforces `--cpus`/`--memory`
  directly. The wrapper *tries* to pin `--cpuset-cpus 0-7`.
  - **Rootless cpuset caveat:** rootless podman only gets the cgroup controllers systemd
    delegates to your user slice, and **`cpuset` is usually not delegated** — so pinning fails
    and the wrapper falls back to a `--cpus` quota (the guest then still *sees* all host cores).
    For true 8‑core fidelity (`nproc == 8` in the guest), do one of:
    - **Delegate cpuset** (one‑time, root): create `/etc/systemd/system/user@.service.d/delegate.conf`

          [Service]
          Delegate=cpu cpuset io memory pids

      then `sudo systemctl daemon-reload` and re‑login.
    - **Run rootful:** `sudo ENGINE=podman CPUSET=0-7 tools/bot_swarm/reference-env/run-container.sh`.
    - **Use the VM** (below) — it genuinely has 8 vCPUs, no cgroup delegation needed.
- **VM:** `vagrant` + `vagrant-libvirt` (`vagrant up --provider=libvirt`, uses KVM). If you'd
  rather not install Vagrant, the container path is the lighter equivalent.

### Windows
- **Container (recommended):** Docker Desktop (WSL2 backend). Size the WSL2 VM in
  `%UserProfile%\.wslconfig` to **at least** the reference, then run `run-container.ps1`:

        [wsl2]
        processors=8
        memory=16GB

  `--cpus`/`--memory` then act as a quota within that VM. (`--cpuset-cpus` is omitted on Windows
  — pinning through the WSL2 layer isn't meaningful.)
- **VM:** Vagrant + VirtualBox or Hyper‑V (`vagrant up --provider=virtualbox`).

### macOS
- **Container:** `podman machine` or Docker Desktop both run a Linux VM — size it to the
  reference first, then run `run-container.sh`:

        podman machine init --cpus 8 --memory 16384 && podman machine start

- **VM:** Vagrant with VMware Fusion / Parallels / UTM / Lima.
- **Apple Silicon caveat:** the guest is **arm64**, not x86‑64. Numbers are valid for
  *relative* scaling (knee location, pattern decomposition) but are not directly comparable to
  an x86 cloud instance. Don't use x86 emulation (Rosetta/qemu) for perf — it distorts results.

## GNS legs on Fedora (container + VM)

`vm-provision.sh` installs `openssl-devel protobuf-devel protobuf-compiler`, and the `Containerfile`
installs neither — so **the container cannot build the GNS profiles as shipped**, and on Fedora the
VM's package list is not sufficient either. `cmake/dependencies.cmake` sets
`Protobuf_USE_STATIC_LIBS ON` (a release must not link `libprotobuf.so` dynamically, #905) and
**Fedora ships no `libprotobuf.a`** — so `find_package(Protobuf)` fails, GNS is *silently*
force-disabled, and the build warns that the packages are missing when they are in fact installed.

Until that is resolved, a local GNS build here needs the shared library pointed at explicitly. This
is fine for a measurement binary that never leaves the machine, and is exactly what the static
preference exists to prevent for a *shipped* one:

    cmake -S . -B /tmp/fl-ref-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON -DFL_ENABLE_GNS=ON \
        -DProtobuf_LIBRARY=/usr/lib64/libprotobuf.so \
        -DProtobuf_LIBRARY_RELEASE=/usr/lib64/libprotobuf.so \
        -DProtobuf_INCLUDE_DIR=/usr/include -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc
    # Then ALWAYS assert it stuck, exactly as scale-gate.yml does — a GNS leg that measured enet6
    # would pass and mean nothing:
    grep -qx 'FL_ENABLE_GNS:BOOL=ON' /tmp/fl-ref-build/CMakeCache.txt

The container also needs the protobuf **runtime** present at run time, not just at build time — a
`--rm` container that built the binary is gone by the time the binary runs.

## Tuning knobs (env vars)

| Var | Default | Meaning |
|---|---|---|
| `CPUS` | `8` | CPU quota |
| `CPUSET` | `0-7` | pinned logical CPUs (Linux/macOS container) |
| `MEM` | `16g` | memory cap |
| `CLIENTS` | `64 128 256` | client counts to sweep |
| `DURATION` | `30` | soak seconds per run |
| `PATTERNS` | `idle weave` | flight patterns to sweep |
| `ENTITY_COUNTS` | `0` | server-side AI entities to pre-spawn per run (#573 pool/index scaling; `FL_TEST_SPAWN_AI`) |
| `SIM_WORKERS` | *(server default)* | data-parallel sim worker counts to sweep (#511; `FL_SIM_WORKER_THREADS`) |

## Topology caveats (read before trusting absolute numbers)

- **Hybrid CPUs (Intel P/E cores).** On a 12th‑gen+ Intel chip, logical CPUs are a mix of
  performance and efficiency cores. `--cpuset-cpus 0-7` should map to 8 **P‑core** threads so the
  guest sees uniform cores like a cloud vCPU — verify with `lscpu -e` and adjust `CPUSET`.
- **Hyper‑threading.** "8 vCPU" cloud instances are usually 4 physical cores × 2 threads. The
  default `0-7` matches that. For 8 *physical* cores, pin one thread per core.
- **Container vs VM.** A cgroup‑throttled container time‑slices on the host scheduler; a VM gets
  dedicated vCPUs. Expect the VM to be slightly more stable/representative; use it to confirm a
  knee the container finds.

## Interpreting results

The scale‑gate targets (per [docs/developer/load-testing.md](../../../docs/developer/load-testing.md)): **128 clients
@ 60 Hz, sim tick ≤ 16.6 ms p99 (observed tick‑Hz ≈ 60), ≤ ~150 KB/s/client** downstream,
soak‑stable. Watch **observed server tick‑Hz min** fall away from 60 as you sweep up — the knee
is the ceiling. Run `idle` (overhead floor) and `weave`/`aggressive` (with physics) to separate
the snapshot‑bandwidth ceiling (Epic B) from the sim ceiling (Epic A).

This environment is also where the **strict** tier of the CI scale gate
([scale-gate.yml](../../../.github/workflows/scale-gate.yml)) belongs: the `reference`/`soak`
profiles' `≤ 16.6 ms p99` tick assertion is only meaningful on this pinned 8‑core/16 GB profile. On
hosted GitHub runners the same profiles run but the tick‑ms gate is advisory (the box isn't
comparable). Registering this VM as a self‑hosted runner that enforces it is set up in
[Self-hosted reference runner](#self-hosted-reference-runner-ci-strict-tier) below (#569). Run it
manually here any time:

    python3 tools/bot_swarm/scale_gate.py --profile reference --build-dir /tmp/fl-ref-build --strict

## Self-hosted reference runner (CI strict tier)

The scale gate's `≤ 16.6 ms p99` tick assertion (`scale_gate.py --strict`) is only trustworthy on
this pinned 8‑core/16 GB profile. To enforce it in CI, register **this reference VM** as a repo
self-hosted runner and dispatch the strict tier at it. Self-host only, no first-party infra (per the
architecture decision record). Setup is scripted in
[`setup-self-hosted-runner.sh`](setup-self-hosted-runner.sh) — the version-controlled form of the
runbook below.

**The box is the reference VM on the dev machine** — the same libvirt guest this directory already
builds (8 vCPU / 16 GB, no hardware to acquire). Prereq is the Linux `vagrant-libvirt` path above.

### Register

1. Get a short-lived **registration token**: repo → Settings → Actions → Runners → *New self-hosted
   runner*. It expires in ~1 h and is never committed.
2. Bring the VM up with the runner provisioner on (one command):

        FL_REFENV_RUNNER=1 FL_RUNNER_TOKEN=<token> vagrant up --provider=libvirt

   Or, if the VM already exists, run the setup script inside it:

        vagrant ssh -c 'sudo FL_RUNNER_TOKEN=<token> bash /src/tools/bot_swarm/reference-env/setup-self-hosted-runner.sh'

3. Verify: `vagrant ssh -c 'sudo /opt/actions-runner/svc.sh status'` shows *active*, and the runner
   shows **Idle** in repo Settings → Runners with labels `self-hosted, linux, x64, fl-reference`.
4. Flip the CI on-switch: set repo variable `FL_REFERENCE_RUNNER_READY = true` (Settings → Actions →
   Variables). The `reference-gate` job stays skipped until this is set.

The runner is **non-ephemeral** — a warm box gives comparable benchmark timing across runs. The
per-job cleanup hook ([`runner-job-cleanup.sh`](runner-job-cleanup.sh)) reclaims the Release build
tree between jobs so non-ephemeral state doesn't accumulate.

### Run the strict tier

Runs are **manual** (`workflow_dispatch`) while this machine isn't 24/7 — GitHub does not queue
missed cron runs, so scheduling on an intermittently-on box would silently skip. Trigger the *Scale
Gate* workflow from the Actions tab (or `gh workflow run scale-gate.yml -f profile=reference`),
picking a profile: `reference`, `soak`, `overrun`, `entity-scale`, or `nightly` (= reference +
overrun). Cron scheduling is deferred until an always-on runner exists — the crons are preserved as
commented lines in the workflow for a one-line re-enable.

`virsh autostart <domain>` (optional) brings the VM up with the host so the runner reconnects
whenever the machine is on.

### Security (public repo + self-hosted)

A self-hosted runner on a public repo must never execute fork-PR code. The layers:

- **Trigger guard** — `reference-gate` is `workflow_dispatch`-only (write-access-gated) behind
  `github.repository == 'fighters-legacy/fighters-legacy'`. No `pull_request`/fork path reaches it;
  the PR tier stays on hosted runners.
- **The VM is the sandbox** — a compromise's blast radius is a disposable guest, not the host. The
  one-way rsync synced folder + NAT networking keep the guest off the host filesystem.
- **Dedicated unprivileged user** (`flrunner`, no login shell) + a systemd hardening drop-in
  ([`runner-hardening.conf`](runner-hardening.conf): `ProtectSystem=strict`, `ProtectHome`,
  `PrivateTmp`, `NoNewPrivileges`).
- Set repo → Settings → Actions → *Fork pull request workflows* → **require approval for all
  outside collaborators**.

### Unregister / off-switch

Set `FL_REFERENCE_RUNNER_READY = false` (or delete it) to disable the CI job immediately. To remove
the runner entirely:

    vagrant ssh -c 'cd /opt/actions-runner && sudo ./svc.sh uninstall && sudo -u flrunner ./config.sh remove --token <removal-token>'

or `vagrant destroy` to tear down the whole VM.
