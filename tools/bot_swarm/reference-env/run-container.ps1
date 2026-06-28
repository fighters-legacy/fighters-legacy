# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-container.ps1 — Windows host wrapper (Docker Desktop). Builds the reference image and runs
# the load sweep constrained to ~8 CPUs / 16 GB.
#
# NOTE: Windows Docker Desktop (WSL2) honours --cpus/--memory as a *quota* on the WSL2 Linux VM,
# which must itself be sized >= 8 CPUs / 16 GB via %UserProfile%\.wslconfig (see README).
# --cpuset-cpus pinning is not used here (less meaningful through the WSL2 layer).
#
# Knobs (env): CPUS (default 8), MEM (default 16g), CLIENTS, DURATION, PATTERNS, IMAGE.
$ErrorActionPreference = "Stop"

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = (Resolve-Path (Join-Path $Here "..\..\..")).Path

$Cpus  = if ($env:CPUS)     { $env:CPUS }     else { "8" }
$Mem   = if ($env:MEM)      { $env:MEM }      else { "16g" }
$Image = if ($env:IMAGE)    { $env:IMAGE }    else { "fl-bot-swarm-refenv" }
$Clients  = if ($env:CLIENTS)  { $env:CLIENTS }  else { "64 128 256" }
$Duration = if ($env:DURATION) { $env:DURATION } else { "30" }
$Patterns = if ($env:PATTERNS) { $env:PATTERNS } else { "idle weave" }

Write-Host "=== building image (docker) ==="
docker build -t $Image -f (Join-Path $Here "Containerfile") $Here

Write-Host "=== running sweep: $Cpus CPUs / $Mem ==="
docker run --rm -t `
    --cpus $Cpus --memory $Mem --memory-swap $Mem `
    -e CLIENTS="$Clients" -e DURATION="$Duration" -e PATTERNS="$Patterns" `
    -v "${Repo}:/src" `
    $Image bash /src/tools/bot_swarm/reference-env/run-benchmark.sh

Write-Host "Reports: $Repo\tools\bot_swarm\results\"
