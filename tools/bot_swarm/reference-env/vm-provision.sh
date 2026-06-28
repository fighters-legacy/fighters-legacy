#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vm-provision.sh — installs the toolchain in the reference VM (called by Vagrant). It does NOT
# run the benchmark; do that after provisioning so you control timing:
#
#   vagrant ssh -c 'sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh'
set -euo pipefail

dnf -y install \
    gcc-c++ cmake ninja-build git \
    SDL3-devel openal-soft-devel \
    dnf-plugins-core findutils procps-ng

# fl-server links SDL3. If the system SDL3 is older than the project's find_package() floor,
# CMake builds SDL3 from source (FetchContent), which needs SDL3's own build deps (alsa/X11/
# wayland/...). Install them so that fallback always succeeds (this is a headless server build).
dnf builddep -y SDL3 || echo "warning: 'dnf builddep SDL3' failed; source-built SDL3 may not configure"

echo "reference VM provisioned: $(nproc) CPUs, $(free -h | awk '/Mem:/{print $2}') RAM"
echo "run: sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh"
