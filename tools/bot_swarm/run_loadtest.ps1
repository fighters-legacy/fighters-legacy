# SPDX-License-Identifier: GPL-3.0-or-later
#
# run_loadtest.ps1 — Windows mirror of run_loadtest.sh. Launches an fl-server with a load-test
# config, runs bot_swarm against it, and captures a JSON report. Mirrors the per-platform pattern
# of tools/latency_analysis/measure_windows.ps1.
#
# Usage: .\run_loadtest.ps1 [BUILD_DIR] [CLIENTS] [DURATION] [PATTERN] [-- <extra bot_swarm args>]
#   BUILD_DIR  build tree containing the binaries (default: build\debug-msvc)
#   CLIENTS    synthetic client count (default: 128)
#   DURATION   soak seconds (default: 30)
#   PATTERN    weave|level|aggressive|idle|random (default: weave)
#   --         everything after a literal `--` is forwarded verbatim to bot_swarm; this is how the
#              scale gate (scale_gate.py, #520) injects the --assert-* flags.
#
# The server's connect-rate-limit and per-IP caps come ONLY from server.toml, so this writes a
# load-test config and points fl-server at it via FL_CONFIG (which never overwrites an existing
# file). Requires the raised server scale ceilings (max_peers up to 1024).
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$BuildDir = "",
    [Parameter(Position = 1)][int]$Clients = 128,
    [Parameter(Position = 2)][int]$Duration = 30,
    [Parameter(Position = 3)][string]$Pattern = "weave",
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "build\debug-msvc" }

# Everything after a literal `--` is forwarded to bot_swarm.
$ExtraArgs = @()
$dashIdx = [Array]::IndexOf($Rest, "--")
if ($dashIdx -ge 0) { $ExtraArgs = $Rest[($dashIdx + 1)..($Rest.Count - 1)] }

$Port = if ($env:FL_LOADTEST_PORT) { [int]$env:FL_LOADTEST_PORT } else { 4793 }

$FlServer = Join-Path $BuildDir "server\fl-server\fl-server.exe"
$BotSwarm = Join-Path $BuildDir "tools\bot_swarm.exe"
if (-not (Test-Path $FlServer)) { Write-Error "fl-server not found at $FlServer" }
if (-not (Test-Path $BotSwarm)) { Write-Error "bot_swarm not found at $BotSwarm" }

# Headroom on max_peers so the harness can also probe past the requested count.
$MaxPeers = [Math]::Min($Clients + 16, 1024)

$WorkDir = New-Item -ItemType Directory -Force -Path (Join-Path $env:TEMP "fl_loadtest_$([Guid]::NewGuid().ToString('N'))")
$Config  = Join-Path $WorkDir "server.toml"
$Metrics = Join-Path $WorkDir "server_tick.json"
$ResultsDir = Join-Path $ScriptDir "results"
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
# FL_LOADTEST_REPORT lets a caller (scale_gate.py) pin the exact report path; default is timestamped.
if ($env:FL_LOADTEST_REPORT) {
    $Report = $env:FL_LOADTEST_REPORT
} else {
    $Timestamp = (Get-Date -Format "yyyyMMddTHHmmssZ")
    $Report = Join-Path $ResultsDir "loadtest_${Clients}c_${Pattern}_$Timestamp.json"
}

# Optional entity-scale knobs (#573), default off so a normal run is byte-identical to before.
$TestSpawnAi = if ($env:FL_TEST_SPAWN_AI) { [int]$env:FL_TEST_SPAWN_AI } else { 0 }
$TestSpawnSpreadKm = if ($env:FL_TEST_SPAWN_SPREAD_KM) { $env:FL_TEST_SPAWN_SPREAD_KM } else { "50" }

# Single-quoted TOML literal strings so Windows backslashes in paths are not treated as escapes.
$ConfigText = @"
[server]
port = $Port
bind_address = "127.0.0.1"
max_peers = $MaxPeers

[security]
connect_rate_limit_count = 100000
connect_rate_limit_window_s = 1
pre_handshake_rate_limit_count = 0
packet_flood_multiplier = 3
max_connections_per_ip = 0

[world]
# The scale gate measures RAW sim/bandwidth capacity against the committed baseline, so the graceful
# tick-overrun governor (#514) is disabled here — otherwise it would shed snapshot/AI work under load
# and mask the very regressions the gate exists to catch. The governor defaults ON in production.
overrun_governor_enabled = false
# Entity-scale load-spawn (#573). 0 = disabled (normal run).
test_spawn_ai_count = $TestSpawnAi
test_spawn_spread_km = $TestSpawnSpreadKm
"@
# Only emit snapshot_budget_bytes when explicitly requested (else keep the server default).
if ($env:FL_SNAPSHOT_BUDGET) {
    $ConfigText += "`nsnapshot_budget_bytes = $($env:FL_SNAPSHOT_BUDGET)"
}
$ConfigText += @"

[metrics]
tick_json_path = '$Metrics'
tick_json_interval_ms = 250
"@
$ConfigText | Set-Content -Path $Config -Encoding UTF8

# FL_SIM_WORKER_THREADS sweeps the data-parallel sim worker count without editing config (#511/#573).
$SrvArgs = @("$Port", "$MaxPeers", "--bind", "127.0.0.1")
if ($env:FL_SIM_WORKER_THREADS) { $SrvArgs += @("--sim-worker-threads", "$($env:FL_SIM_WORKER_THREADS)") }

Write-Host "=== bot_swarm load test: $Clients clients, pattern=$Pattern, ${Duration}s, port $Port (test_spawn_ai=$TestSpawnAi) ==="
$env:FL_CONFIG = $Config
$SrvProc = Start-Process -FilePath $FlServer `
    -ArgumentList $SrvArgs `
    -PassThru -NoNewWindow

try {
    Start-Sleep -Seconds 2
    if ($SrvProc.HasExited) { Write-Error "fl-server exited during startup" }

    & $BotSwarm 127.0.0.1 $Port `
        --clients $Clients --duration $Duration --pattern $Pattern `
        --json $Report --server-metrics $Metrics @ExtraArgs
    $Status = $LASTEXITCODE

    if (-not (Select-String -Path $Report -Pattern '"server_tick"' -Quiet)) {
        Write-Error "report $Report is missing the authoritative server_tick block"
    }

    Write-Host "Report: $Report"
    exit $Status
}
finally {
    if (-not $SrvProc.HasExited) { $SrvProc.Kill() }
    Remove-Item -Recurse -Force $WorkDir -ErrorAction SilentlyContinue
}
