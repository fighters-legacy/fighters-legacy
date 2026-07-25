# SPDX-License-Identifier: GPL-3.0-or-later
#
# measure_windows.ps1 — LLM inference vs Vulkan renderer GPU contention, Windows leg (#782).
#
# Windows mirror of measure_linux.sh; see that script for the method. #782 names BOTH Windows
# inference backends, so this is normally run twice on the same machine:
#
#   CUDA   (Ollama, the default):
#     .\measure_windows.ps1 -Model qwen2.5-coder:14b -Label cuda
#
#   Vulkan (llama.cpp's llama-server built with -DGGML_VULKAN=ON, serving OpenAI-compatible):
#     .\measure_windows.ps1 -BaseUrl http://localhost:8080 -Model local -Label vulkan
#
# The Vulkan case is the interesting one: there the inference backend and the renderer are on the
# same API and the same queue family, which is the configuration most likely to contend.
#
# BEFORE RUNNING: pin the model in memory ($env:OLLAMA_KEEP_ALIVE = "-1", or llama-server's
# equivalent). An evicted model reloads INSIDE a burst — a 14B costs ~55 s to load (#769) — and
# that load would be measured as contention rather than as what it is.
[CmdletBinding()]
param(
    [string]$Model = "qwen2.5-coder:14b",
    [string]$BaseUrl = $(if ($env:FL_AI_BASE_URL) { $env:FL_AI_BASE_URL } else { "http://localhost:11434" }),
    [ValidateSet("intent", "mission", "ops")][string]$Workload = "intent",
    [string]$Mission = "builtin:sandbox",
    [int]$Concurrency = 1,
    [int]$Bursts = 5,
    [double]$BurstSeconds = 20,
    [double]$IdleSeconds = 60,
    [double]$GapSeconds = 20,
    [double]$TailSeconds = 30,
    [double]$SettleSeconds = 60,
    [string]$Label = "",
    [Parameter(Position = 0)][string]$BuildDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
# `release` is the Clang/GCC-on-Windows preset; `release-msvc` is the one docs/development.md tells
# Windows developers to build, so fall back to it rather than making -BuildDir mandatory here.
if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot "build\release"
    if (-not (Test-Path (Join-Path $BuildDir "game"))) {
        $msvc = Join-Path $RepoRoot "build\release-msvc"
        if (Test-Path (Join-Path $msvc "game")) { $BuildDir = $msvc }
    }
}

$Port = if ($env:FL_CONTENTION_PORT) { $env:FL_CONTENTION_PORT } else { "4796" }
$Game = Join-Path $BuildDir "game\fighters-legacy\fighters-legacy.exe"
$FlServer = Join-Path $BuildDir "server\fl-server\fl-server.exe"
$Results = Join-Path $ScriptDir "results"
$Stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
$Suffix = if ($Label) { "windows_${Label}_$Stamp" } else { "windows_$Stamp" }
New-Item -ItemType Directory -Force -Path $Results | Out-Null

# MSVC multi-config generators nest binaries under a per-config directory.
if (-not (Test-Path $Game)) { $Game = Join-Path $BuildDir "game\fighters-legacy\Release\fighters-legacy.exe" }
if (-not (Test-Path $FlServer)) { $FlServer = Join-Path $BuildDir "server\fl-server\Release\fl-server.exe" }
if (-not (Test-Path $Game)) { throw "game binary not found under $BuildDir (build a Release tree first)" }
if (-not (Test-Path $FlServer)) { throw "fl-server not found under $BuildDir" }

# ── Preflight: the endpoint must answer ──────────────────────────────────────────────────────
$endpointOk = $false
foreach ($probe in @("/v1/models", "/api/tags")) {
    try {
        Invoke-WebRequest -Uri ($BaseUrl.TrimEnd("/") + $probe) -TimeoutSec 5 -UseBasicParsing | Out-Null
        $endpointOk = $true
        break
    }
    catch { }
}
if (-not $endpointOk) { throw "no OpenAI-compatible endpoint answering at $BaseUrl" }

# ── Python launcher ──────────────────────────────────────────────────────────────────────────
# Bare `python` is not a safe assumption on Windows: a stock box resolves it to the Microsoft
# Store app-execution alias, which prints "Python was not found" and exits WITHOUT running the
# script. Probe each candidate by actually executing it and keep the first that returns 0.
$PythonCmd = $null
foreach ($cand in @(
        @{ Exe = "py"; Pre = @("-3") },
        @{ Exe = "python3"; Pre = @() },
        @{ Exe = "python"; Pre = @() }
    )) {
    $found = Get-Command $cand.Exe -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
    if (-not $found) { continue }
    & $found.Source @($cand.Pre + "--version") *> $null
    if ($LASTEXITCODE -eq 0) { $PythonCmd = @{ Exe = $found.Source; Pre = $cand.Pre }; break }
}
if (-not $PythonCmd) { throw "no working Python interpreter found (tried: py -3, python3, python)" }

# The driver owns the schedule arithmetic; the game's run length is derived from it rather than
# recomputed here, so the two cannot drift apart.
$ScheduleS = [double](& $PythonCmd.Exe @($PythonCmd.Pre) "$ScriptDir\driver.py" --model $Model --print-schedule `
        --idle-seconds $IdleSeconds --bursts $Bursts --burst-seconds $BurstSeconds `
        --gap-seconds $GapSeconds --tail-seconds $TailSeconds)
# Margin covers connect + terrain streaming before the first Flight frame, the settle window below,
# and the warm-up probe (which may be a full cold model load) that runs before the schedule starts.
$RunSeconds = [int]$ScheduleS + [int]$SettleSeconds + 150

Write-Host "-- GPU contention (#782), Windows --"
Write-Host "  endpoint : $BaseUrl   model: $Model   workload: $Workload (concurrency $Concurrency)"
Write-Host "  scene    : $Mission   schedule: ${ScheduleS}s   game run: ${RunSeconds}s"
Write-Host "  results  : $Results\*_$Suffix.*"

# ── System info + VRAM before anything is loaded ─────────────────────────────────────────────
$SysInfo = Join-Path $Results "sysinfo_$Suffix.txt"
$lines = @("date_utc: $Stamp")
$os = Get-CimInstance Win32_OperatingSystem
$lines += "os: $($os.Caption) $($os.Version)"
$lines += "cpu: $((Get-CimInstance Win32_Processor | Select-Object -First 1).Name)"
$lines += "mem_total_kb: $($os.TotalVisibleMemorySize)"
$nvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if ($nvidiaSmi) {
    $lines += "gpu: $(& nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader)"
    $lines += "vram_before_mb: $(& nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)"
    $lines += "compute_apps_before: $((& nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader) -join ';')"
}
else {
    # Non-NVIDIA: the per-process dedicated-usage counter is the closest equivalent.
    $lines += "gpu: $((Get-CimInstance Win32_VideoController | Select-Object -First 1).Name)"
    try {
        $ded = (Get-Counter '\GPU Process Memory(*)\Dedicated Usage' -ErrorAction Stop).CounterSamples |
        Where-Object { $_.CookedValue -gt 0 } | Measure-Object -Property CookedValue -Sum
        $lines += "gpu_dedicated_before_mb: $([math]::Round($ded.Sum / 1MB, 1))"
    }
    catch { $lines += "gpu_dedicated_before_mb: unavailable" }
}
$lines | Set-Content -Path $SysInfo -Encoding utf8
Write-Host "  sysinfo  : $SysInfo"

$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("flgpu_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$ServerLog = Join-Path $WorkDir "server.log"
$GameLog = Join-Path $WorkDir "game.log"
$FramesJson = Join-Path $Results "frames_$Suffix.json"
$DriverJson = Join-Path $Results "driver_$Suffix.json"
$serverProc = $null
$gameProc = $null

try {
    # ── Boot the server ──────────────────────────────────────────────────────────────────────
    $serverProc = Start-Process -FilePath $FlServer `
        -ArgumentList @($Port, "8", "--bind", "127.0.0.1", "--mission", $Mission) `
        -RedirectStandardOutput $ServerLog -RedirectStandardError "$ServerLog.err" `
        -NoNewWindow -PassThru
    $up = $false
    for ($i = 0; $i -lt 50; $i++) {
        if ((Test-Path $ServerLog) -and (Select-String -Path $ServerLog -Pattern "listening on" -Quiet)) {
            $up = $true
            break
        }
        if ($serverProc.HasExited) { throw "fl-server exited early: $(Get-Content $ServerLog -Tail 5)" }
        Start-Sleep -Milliseconds 200
    }
    if (-not $up) { throw "fl-server never came up: $(Get-Content $ServerLog -Tail 5)" }

    # ── Launch the game ──────────────────────────────────────────────────────────────────────
    # Observer, not pilot: a spectator ghost holds a fixed camera over a streamed-in scene, so the
    # render load is repeatable. Windowed, not --headless: present and the compositor are part of
    # what contends for the GPU.
    $gameProc = Start-Process -FilePath $Game `
        -ArgumentList @("--connect", "127.0.0.1:$Port", "--observer", "--auto",
        "--frame-stats-json", $FramesJson, "--run-seconds", $RunSeconds) `
        -RedirectStandardOutput $GameLog -RedirectStandardError "$GameLog.err" `
        -PassThru

    Write-Host "  waiting for the client to reach Flight and start recording..."
    $recording = $false
    for ($i = 0; $i -lt 150; $i++) {
        if ((Test-Path $FramesJson) -and ((Get-Item $FramesJson).Length -gt 0)) {
            $recording = $true
            break
        }
        if ($gameProc.HasExited) { throw "game exited early: $(Get-Content $GameLog -Tail 20)" }
        Start-Sleep -Seconds 1
    }
    if (-not $recording) { throw "no frame stats after 150 s: $(Get-Content $GameLog -Tail 20)" }

    # ── Let the client settle before the baseline starts ─────────────────────────────────────
    # The first stats flush arrives ~5 s into Flight, but the client is not in steady state yet:
    # a 240 Hz run on this reference box showed three multi-second episodes of ~11 ms frames
    # scattered through the first ~55 s (terrain still streaming, pipelines still warming), and
    # then nothing for the rest of the run. Starting the driver at first-flush folds those into
    # the idle baseline, which is the one number every burst is compared against -- it inflated
    # idle p95 to 11 ms against a 4.18 ms mean and made bursts look *faster* than idle.
    # analyze.py's --settle-s only trims phase boundaries; it cannot fix a baseline placed on top
    # of startup noise.
    if ($SettleSeconds -gt 0) {
        Write-Host "  settling for ${SettleSeconds}s before the baseline..."
        Start-Sleep -Seconds $SettleSeconds
    }

    # ── Drive the bursts ─────────────────────────────────────────────────────────────────────
    & $PythonCmd.Exe @($PythonCmd.Pre) "$ScriptDir\driver.py" `
        --base-url $BaseUrl --model $Model --workload $Workload --concurrency $Concurrency `
        --idle-seconds $IdleSeconds --bursts $Bursts --burst-seconds $BurstSeconds `
        --gap-seconds $GapSeconds --tail-seconds $TailSeconds --out $DriverJson
    if ($LASTEXITCODE -ne 0) { throw "driver.py failed with exit code $LASTEXITCODE" }

    # ── VRAM at peak load (model resident, game rendering) ────────────────────────────────────
    if ($nvidiaSmi) {
        Add-Content -Path $SysInfo -Value "vram_after_mb: $(& nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)"
        Add-Content -Path $SysInfo -Value "compute_apps_after: $((& nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader) -join ';')"
    }

    Write-Host "  waiting for the game to finish its run..."
    $gameProc.WaitForExit()
}
finally {
    foreach ($p in @($gameProc, $serverProc)) {
        # The game's periodic flush means a killed process still left a usable frame-stats file.
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item -Recurse -Force $WorkDir -ErrorAction SilentlyContinue
}

$outPrefix = if ($Label) { Join-Path $Results "windows_${Label}_$Stamp" } else { Join-Path $Results "windows_$Stamp" }
& $PythonCmd.Exe @($PythonCmd.Pre) "$ScriptDir\analyze.py" --frame-stats $FramesJson --driver $DriverJson --scene $Mission --out $outPrefix
exit $LASTEXITCODE
