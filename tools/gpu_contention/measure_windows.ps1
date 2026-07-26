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
#     .\measure_windows.ps1 -BaseUrl http://127.0.0.1:8081 -Model local -Label vulkan
#
# USE 127.0.0.1, NEVER `localhost` (#1021). Both inference servers bind IPv4 only, but `localhost`
# resolves to ::1 first on Windows; the IPv6 connect has to time out before Python falls back, and
# that costs ~2.1 s of DEAD WAIT on EVERY request. It does not fail, so nothing looks wrong -- the
# burst simply serves ~9 requests instead of ~75 and spends ~85% of its window idle, which is a
# near-silent way to measure almost no contention and report it as a result. Every Windows run
# before #1021 hit this (45 requests/run, 2.3 s/request, against 0.27 s/request once fixed).
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
    # 127.0.0.1, not localhost -- see the header note: localhost costs ~2.1 s/request here.
    [string]$BaseUrl = $(if ($env:FL_AI_BASE_URL) { $env:FL_AI_BASE_URL } else { "http://127.0.0.1:11434" }),
    [ValidateSet("intent", "mission", "ops")][string]$Workload = "intent",
    [string]$Mission = "builtin:sandbox",
    [int]$Concurrency = 1,
    [int]$Bursts = 5,
    [double]$BurstSeconds = 20,
    [double]$IdleSeconds = 60,
    [double]$GapSeconds = 20,
    [double]$TailSeconds = 30,
    [double]$SettleSeconds = 60,
    [int]$Repeat = 1,
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
$Cell = if ($Label) { "windows_$Label" } else { "windows" }
$Suffix = "${Cell}_$Stamp"
New-Item -ItemType Directory -Force -Path $Results | Out-Null

# MSVC multi-config generators nest binaries under a per-config directory.
if (-not (Test-Path $Game)) { $Game = Join-Path $BuildDir "game\fighters-legacy\Release\fighters-legacy.exe" }
if (-not (Test-Path $FlServer)) { $FlServer = Join-Path $BuildDir "server\fl-server\Release\fl-server.exe" }
if (-not (Test-Path $Game)) { throw "game binary not found under $BuildDir (build a Release tree first)" }
if (-not (Test-Path $FlServer)) { throw "fl-server not found under $BuildDir" }

# ── Preflight: the endpoint must answer ──────────────────────────────────────────────────────
# An explicit -BaseUrl beats the corrected default, and the #1021 runbook itself said `localhost`,
# so warn rather than silently letting the ~2.1 s/request IPv6 stall gut the burst load.
if ($BaseUrl -match "localhost") {
    Write-Host "  WARNING: -BaseUrl uses 'localhost'. On Windows that resolves to ::1 first and costs"
    Write-Host "           ~2.1 s of dead wait per request against an IPv4-only server (#1021)."
    Write-Host "           Use http://127.0.0.1:<port> instead, or the bursts will be mostly idle."
}
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
Write-Host "  scene    : $Mission   schedule: ${ScheduleS}s   game run: ${RunSeconds}s   repeat: $Repeat"
Write-Host "  results  : $Results\*_$Suffix*.*"

# ── System info + VRAM before anything is loaded ─────────────────────────────────────────────
$SysInfo = Join-Path $Results "sysinfo_$Suffix.txt"
$lines = @("date_utc: $Stamp")
$os = Get-CimInstance Win32_OperatingSystem
$lines += "os: $($os.Caption) $($os.Version)"
$lines += "cpu: $((Get-CimInstance Win32_Processor | Select-Object -First 1).Name)"
$lines += "mem_total_kb: $($os.TotalVisibleMemorySize)"
$nvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue

# Non-NVIDIA per-process GPU memory (dedicated + shared), summed across processes, in MB. Intel
# integrated graphics is UNIFIED memory, so a resident model + the renderer show up mostly under
# Shared Usage, not Dedicated -- capture both or the Intel run records nothing useful. Best-effort:
# these perf counters can be localized or absent, in which case the field reads "unavailable".
function Get-GpuMemLine {
    param([string]$Phase)
    $parts = @()
    foreach ($ctr in @('Dedicated Usage', 'Shared Usage')) {
        try {
            $sum = (Get-Counter "\GPU Process Memory(*)\$ctr" -ErrorAction Stop).CounterSamples |
                Where-Object { $_.CookedValue -gt 0 } | Measure-Object -Property CookedValue -Sum
            $parts += "$($ctr.Split(' ')[0].ToLower())=$([math]::Round(($sum.Sum) / 1MB, 1))MB"
        }
        catch { $parts += "$($ctr.Split(' ')[0].ToLower())=unavailable" }
    }
    return "gpu_mem_${Phase}: $($parts -join ' ')"
}

if ($nvidiaSmi) {
    $lines += "gpu: $(& nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader)"
    $lines += "vram_before_mb: $(& nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)"
    $lines += "compute_apps_before: $((& nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader) -join ';')"
}
else {
    $lines += "gpu: $((Get-CimInstance Win32_VideoController | Select-Object -First 1).Name)"
    $lines += (Get-GpuMemLine 'before')
}
$lines | Set-Content -Path $SysInfo -Encoding utf8
Write-Host "  sysinfo  : $SysInfo"

$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("flgpu_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$ServerLog = Join-Path $WorkDir "server.log"
$serverProc = $null
$gameProc = $null
$reports = @()
$warnedRuns = @()

# One measurement pass: fresh game, settle, drive, per-run analyze. Sets $script:gameProc so the
# finally block can reap whatever is live, and appends the run's report JSON path to $script:reports.
# Observer, not pilot: a spectator ghost holds a fixed camera over a streamed-in scene, so the
# render load is repeatable. Windowed, not --headless: present and the compositor contend for the GPU.
function Invoke-Run {
    param([int]$Run)
    $runTag = if ($Repeat -gt 1) { "_run{0:D2}" -f $Run } else { "" }
    $tag = "$Suffix$runTag"
    $frames = Join-Path $Results "frames_$tag.json"
    $driver = Join-Path $Results "driver_$tag.json"
    $reportPrefix = Join-Path $Results "${Cell}_${Stamp}$runTag"
    $gameLog = Join-Path $WorkDir "game_$Run.log"

    $script:gameProc = Start-Process -FilePath $Game `
        -ArgumentList @("--connect", "127.0.0.1:$Port", "--observer", "--auto",
        "--frame-stats-json", $frames, "--run-seconds", $RunSeconds) `
        -RedirectStandardOutput $gameLog -RedirectStandardError "$gameLog.err" `
        -PassThru

    Write-Host "  [run $Run/$Repeat] waiting for the client to reach Flight and start recording..."
    $recording = $false
    for ($i = 0; $i -lt 150; $i++) {
        if ((Test-Path $frames) -and ((Get-Item $frames).Length -gt 0)) { $recording = $true; break }
        if ($script:gameProc.HasExited) { throw "game exited early: $(Get-Content $gameLog -Tail 20)" }
        Start-Sleep -Seconds 1
    }
    if (-not $recording) { throw "no frame stats after 150 s: $(Get-Content $gameLog -Tail 20)" }

    # ── Let the client settle before the baseline starts ─────────────────────────────────────
    # The first stats flush arrives ~5 s into Flight, but the client is not in steady state yet:
    # a 240 Hz run on this reference box showed multi-second episodes of ~11 ms frames scattered
    # through the first ~55 s (terrain still streaming, pipelines still warming). Starting the
    # driver at first-flush folds those into the idle baseline, the one number every burst is
    # compared against. Per-run, because every fresh game launch has this startup transient.
    if ($SettleSeconds -gt 0) {
        Write-Host "  [run $Run/$Repeat] settling for ${SettleSeconds}s before the baseline..."
        Start-Sleep -Seconds $SettleSeconds
    }

    & $PythonCmd.Exe @($PythonCmd.Pre) "$ScriptDir\driver.py" `
        --base-url $BaseUrl --model $Model --workload $Workload --concurrency $Concurrency `
        --idle-seconds $IdleSeconds --bursts $Bursts --burst-seconds $BurstSeconds `
        --gap-seconds $GapSeconds --tail-seconds $TailSeconds --out $driver
    if ($LASTEXITCODE -ne 0) { throw "driver.py failed with exit code $LASTEXITCODE" }

    # GPU memory at peak load (model resident + game rendering) — captured on the first run; the
    # model is resident for all repeats. The non-NVIDIA/Intel path is the load-bearing one here,
    # since the Ollama /api/ps model-VRAM probe reads "--" on llama-server.
    if ($Run -eq 1) {
        if ($nvidiaSmi) {
            Add-Content -Path $SysInfo -Value "vram_after_mb: $(& nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)"
            Add-Content -Path $SysInfo -Value "compute_apps_after: $((& nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader) -join ';')"
        }
        else {
            Add-Content -Path $SysInfo -Value (Get-GpuMemLine 'after')
        }
    }

    Write-Host "  [run $Run/$Repeat] waiting for the game to finish its run..."
    $script:gameProc.WaitForExit()
    $script:gameProc = $null

    $cellLabel = if ($Label) { $Label } else { $Model }
    & $PythonCmd.Exe @($PythonCmd.Pre) "$ScriptDir\analyze.py" --frame-stats $frames --driver $driver `
        --scene $Mission --label $cellLabel --out $reportPrefix
    # analyze.py exits non-zero when it emits warnings — a run whose numbers are not trustworthy must
    # not look like a clean pass. Recorded rather than thrown: throwing on run 3 of 5 would discard
    # the runs that already succeeded AND skip the aggregate, and the distribution is the entire point
    # of --repeat. Every run finishes; the script's exit code carries the fact (see the tail below).
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [run $Run/$Repeat] analyze.py exited $LASTEXITCODE - it emitted warnings; see $reportPrefix.md"
        $script:warnedRuns += $Run
    }
    $script:reports += "$reportPrefix.json"
}

try {
    # ── Boot the server ONCE; all repeats run against the same warm server + model ─────────────
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

    for ($run = 1; $run -le $Repeat; $run++) { Invoke-Run -Run $run }
}
finally {
    foreach ($p in @($gameProc, $serverProc)) {
        # The game's periodic flush means a killed process still left a usable frame-stats file.
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item -Recurse -Force $WorkDir -ErrorAction SilentlyContinue
}

# ── Aggregate (only meaningful for repeated runs) ─────────────────────────────────────────────
if ($Repeat -gt 1) {
    Write-Host "-- aggregate of $Repeat runs --"
    & $PythonCmd.Exe @($PythonCmd.Pre) "$ScriptDir\aggregate.py" --reports @($reports) `
        --out (Join-Path $Results "${Cell}_${Stamp}_agg")
}
$aggExit = $LASTEXITCODE

if ($warnedRuns.Count -gt 0) {
    Write-Host "-- analyze.py emitted warnings on run(s) $($warnedRuns -join ', ') of $Repeat - read those per-run reports before quoting any of these numbers"
    exit 1
}
exit $aggExit
