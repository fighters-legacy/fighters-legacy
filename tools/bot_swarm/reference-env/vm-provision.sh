#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vm-provision.sh — installs the toolchain in the reference VM (called by Vagrant). It does NOT
# run the benchmark; do that after provisioning so you control timing:
#
#   vagrant ssh -c 'sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh'
set -euo pipefail

# Just the build toolchain (same approach as the Containerfile): the harness builds only fl-server
# (SDL-free platform-stdfs) + bot_swarm (enet6) headless, neither of which links SDL3 — so no SDL3
# build dependencies are needed (dropped after #711/#716 moved fl-server off platform-sdl3; #718).
dnf -y install \
    gcc-c++ cmake ninja-build git \
    findutils procps-ng

echo "reference VM provisioned: $(nproc) CPUs, $(free -h | awk '/Mem:/{print $2}') RAM"
echo "run: sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh"
