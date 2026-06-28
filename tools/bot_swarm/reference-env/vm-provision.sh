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
    findutils procps-ng

echo "reference VM provisioned: $(nproc) CPUs, $(free -h | awk '/Mem:/{print $2}') RAM"
echo "run: sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh"
