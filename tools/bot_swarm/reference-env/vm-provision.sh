#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vm-provision.sh — installs the toolchain in the reference VM (called by Vagrant). It does NOT
# run the benchmark; do that after provisioning so you control timing:
#
#   vagrant ssh -c 'sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh'
set -euo pipefail

# Toolchain + SDL3's build dependencies (same approach as the Containerfile). No SDL3-devel: SDL3
# is built FROM SOURCE (the repo's FetchContent-pinned version) for determinism. `dnf builddep
# SDL3` pulls Fedora's complete SDL3 BuildRequires; libXtst/libXinerama cover the FetchContent SDL3.
dnf -y install \
    gcc-c++ cmake ninja-build git dnf-plugins-core \
    findutils procps-ng
dnf builddep -y SDL3
dnf -y install libXtst-devel libXinerama-devel

echo "reference VM provisioned: $(nproc) CPUs, $(free -h | awk '/Mem:/{print $2}') RAM"
echo "run: sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh"
