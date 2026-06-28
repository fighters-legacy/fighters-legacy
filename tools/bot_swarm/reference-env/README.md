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
`tools/bot_swarm/results/` (git‑ignored). See [docs/load-testing.md](../../../docs/load-testing.md).

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

## Tuning knobs (env vars)

| Var | Default | Meaning |
|---|---|---|
| `CPUS` | `8` | CPU quota |
| `CPUSET` | `0-7` | pinned logical CPUs (Linux/macOS container) |
| `MEM` | `16g` | memory cap |
| `CLIENTS` | `64 128 256` | client counts to sweep |
| `DURATION` | `30` | soak seconds per run |
| `PATTERNS` | `idle weave` | flight patterns to sweep |

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

The scale‑gate targets (per [docs/load-testing.md](../../../docs/load-testing.md)): **128 clients
@ 60 Hz, sim tick ≤ 16.6 ms p99 (observed tick‑Hz ≈ 60), ≤ ~150 KB/s/client** downstream,
soak‑stable. Watch **observed server tick‑Hz min** fall away from 60 as you sweep up — the knee
is the ceiling. Run `idle` (overhead floor) and `weave`/`aggressive` (with physics) to separate
the snapshot‑bandwidth ceiling (Epic B) from the sim ceiling (Epic A).
