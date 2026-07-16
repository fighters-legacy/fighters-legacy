# SPDX-License-Identifier: GPL-3.0-or-later
#
# visual_check.ps1 — Windows counterpart of visual_check.sh: one-command visual verification of
# the builtin placeholder meshes (#886). Default (observer) mode boots a standalone fl-server with
# the builtin:shape-gallery mission, injects wreck-staging `detonate` admin commands into the
# server's stdin after a delay, and opens the game window attached as an observer ghost (no menu
# interaction). -Fly launches pilot mode instead (single-player into the armed player slot).
#
# Usage: .\tools\visual_check.ps1 [-Fly] [-Build] [-Mission <id>] [-BuildDir <dir>]
# Env:   FL_VISUAL_PORT (default 4795); FL_VISUAL_STAGE_DELAY seconds (default 25; 0 disables).
param(
    [switch]$Fly,
    [switch]$Build,
    [string]$Mission = "builtin:shape-gallery",
    [string]$BuildDir = ""
)
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ($BuildDir -eq "") { $BuildDir = Join-Path $RepoRoot "build\debug-msvc" }
$Port = if ($env:FL_VISUAL_PORT) { $env:FL_VISUAL_PORT } else { "4795" }
$StageDelay = if ($env:FL_VISUAL_STAGE_DELAY) { [int]$env:FL_VISUAL_STAGE_DELAY } else { 25 }

if ($Build) { cmake --build --preset debug-msvc; if ($LASTEXITCODE -ne 0) { exit 1 } }

$Game = Join-Path $BuildDir "game\fighters-legacy\fighters-legacy.exe"
$FlServer = Join-Path $BuildDir "server\fl-server\fl-server.exe"
if (-not (Test-Path $Game)) { Write-Error "game binary not found at $Game (build first: -Build)" }

if ($Fly) {
    Write-Host "visual_check: launching pilot mode into $Mission (fire bombs/rockets on stations 4/5)"
    & $Game --mission $Mission
    exit $LASTEXITCODE
}

if (-not (Test-Path $FlServer)) { Write-Error "fl-server not found at $FlServer (build first: -Build)" }

# Standalone server with redirected stdin — the admin command channel for wreck staging.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $FlServer
$psi.Arguments = "$Port 8 --bind 127.0.0.1 --mission `"$Mission`""
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$server = [System.Diagnostics.Process]::Start($psi)

try {
    $up = $false
    for ($i = 0; $i -lt 50 -and -not $server.HasExited; $i++) {
        $line = $server.StandardOutput.ReadLine()
        if ($line -match "listening on") { $up = $true; break }
    }
    if (-not $up) { Write-Error "fl-server never came up" }
    Write-Host "visual_check: fl-server up on 127.0.0.1:$Port with $Mission"

    # Drain the redirected pipes asynchronously so the server never blocks on a full pipe.
    $null = $server.StandardOutput.ReadToEndAsync()
    $null = $server.StandardError.ReadToEndAsync()

    if ($StageDelay -ne 0 -and $Mission -eq "builtin:shape-gallery") {
        Write-Host "visual_check: wreck-staging detonations fire in ${StageDelay}s"
    }

    Write-Host "visual_check: opening observer window (Num1/Num2 cycle entities, F2 chase, F4 free-fly)"
    $client = Start-Process -FilePath $Game -ArgumentList "--connect", "127.0.0.1:$Port", "--observer", "--auto" -PassThru

    if ($StageDelay -ne 0 -and $Mission -eq "builtin:shape-gallery") {
        Start-Sleep -Seconds $StageDelay
        $server.StandardInput.WriteLine("detonate 600 555 -240 60 100")  # ground vehicle -> wreck
        $server.StandardInput.WriteLine("detonate 600 555 -120 60 400")  # structure -> wreck
        $server.StandardInput.WriteLine("detonate 700 555 220 60 2000")  # naval vessel -> wreck
        $server.StandardInput.Flush()
    }

    $client.WaitForExit()
} finally {
    if (-not $server.HasExited) {
        try { $server.StandardInput.WriteLine("quit"); $server.StandardInput.Flush() } catch {}
        if (-not $server.WaitForExit(3000)) { $server.Kill() }
    }
}
