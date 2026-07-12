#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vm-provision.sh — installs the toolchain in the reference VM (called by Vagrant). It does NOT
# run the benchmark; do that after provisioning so you control timing:
#
#   vagrant ssh -c 'sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh'
set -euo pipefail

# Just the build toolchain (same approach as the Containerfile): the harness builds only fl-server
# (SDL-free platform-stdfs) + bot_swarm headless, neither of which links SDL3 — so no SDL3
# build dependencies are needed (dropped after #711/#716 moved fl-server off platform-sdl3; #718).
#
# openssl-devel + protobuf-devel/-compiler are the GameNetworkingSockets deps (#649/#773): every
# reference-runner leg builds FL_ENABLE_GNS=ON — GNS is the DEFAULT internet transport and, since
# #773, the primary `reference` profile plus every characterisation profile pins `transport: gns`.
# GNS v1.6.0 needs the PRE-ABSEIL protobuf 3.21.x line, which Fedora still ships (3.19.x) — see
# docs/gns-backend.md. Without these, cmake/dependencies.cmake silently force-disables GNS and the
# legs would measure enet6; the workflow asserts FL_ENABLE_GNS stayed ON after configure so that
# can't pass silently.
dnf -y install \
    gcc-c++ cmake ninja-build git \
    openssl-devel protobuf-devel protobuf-compiler \
    findutils procps-ng

echo "reference VM provisioned: $(nproc) CPUs, $(free -h | awk '/Mem:/{print $2}') RAM"
echo "run: sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh"
