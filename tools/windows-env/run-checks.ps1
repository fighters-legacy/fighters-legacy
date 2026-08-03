# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-checks.ps1 - runs INSIDE the Windows validation VM (or on the self-hosted Windows runner).
# The Windows counterpart of tools/bot_swarm/reference-env/run-benchmark.sh (#1114).
#
# Three tiers, selectable with -Tiers or FL_WINENV_TIERS (comma-separated):
#
#   ci       mirrors the ci.yml Windows leg      - debug-msvc + vcpkg + GNS assert + build + ctest
#   smoke    mirrors scale-gate.yml windows-smoke - release-msvc (GNS off) + run_loadtest.ps1
#   runtime  no CI equivalent                    - render builtin:shape-gallery to a screenshot
#
# Usage (normally invoked by run-windows-check.sh from the host):
#   run-checks.ps1 [-Sha <sha>] [-Tiers ci,smoke,runtime] [-Source <existing checkout>]
#
# -Source skips all source syncing and runs against a checkout someone else prepared; that is the
# self-hosted-runner path, where actions/checkout has already placed the tree. Without it the
# script maintains its own clone at C:\fl\src and fast-forwards it to -Sha from an uploaded bundle.
#
# Env knobs: FL_WINENV_TIERS, FL_WINENV_SRC (default C:\fl\src), FL_WINENV_OUT (default C:\fl\out),
#            FL_WINENV_GPU (1 = render on the passed-through GPU instead of lavapipe),
#            FL_WINENV_RUNTIME_VIA_TASK (1 = run the runtime tier through a scheduled task).

[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Sha = "",
    [Parameter(Position = 1)][string]$Tiers = "",
    [Parameter(Position = 2)][string]$Source = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Tiers -eq "") {
    $Tiers = if ($env:FL_WINENV_TIERS) { $env:FL_WINENV_TIERS } else { "ci,smoke,runtime" }
}
$Src = if ($Source) { $Source }
       elseif ($env:FL_WINENV_SRC) { $env:FL_WINENV_SRC }
       else { "C:\fl\src" }
$OutDir   = if ($env:FL_WINENV_OUT) { $env:FL_WINENV_OUT } else { "C:\fl\out" }
$UseGpu   = ($env:FL_WINENV_GPU -eq "1")
$ViaTask  = ($env:FL_WINENV_RUNTIME_VIA_TASK -eq "1")
$RepoUrl  = "https://github.com/fighters-legacy/fighters-legacy"
$MesaIcd  = "C:\mesa\x64\lvp_icd.x86_64.json"

$TierList = @($Tiers.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Results  = [ordered]@{}

function Write-Section([string]$Title) { Write-Host ""; Write-Host "=== $Title ===" }

# Native commands do not throw on failure, and a build that fails while the script marches on to
# ctest reports the wrong tier. Every external command goes through this.
function Invoke-Checked {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$CmdArgs = @())
    Write-Host "> $Exe $($CmdArgs -join ' ')"
    & $Exe @CmdArgs
    if ($LASTEXITCODE -ne 0) { throw "$Exe exited with code $LASTEXITCODE" }
}

# ---- MSVC environment ----
# The scripted equivalent of the ilammy/msvc-dev-cmd action the CI legs use: without it `cl` is not
# on PATH in a non-interactive WinRM shell and CMake picks up no compiler at all.
function Enter-MsvcEnvironment {
    if ($env:VSCMD_ARG_TGT_ARCH -eq "x64") { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio Build Tools installed?" }
    $vsPath = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw "no Visual Studio instance with the C++ toolset - re-run provisioning" }
    $vsVer = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null
    Write-Host "msvc environment: $vsPath (version $vsVer, x64)"

    # Say out loud which compiler is predicting CI. A DIFFERENT MAJOR VERSION INVENTS FAILURES: on
    # 14.44 (VS 2022) this repo fails to compile a raw string inside a CHECK() macro that 14.51
    # (VS 2026, what the runner uses) accepts. Someone reading a red tier has to be able to tell
    # "your branch is broken" from "this VM is not the compiler CI runs".
    $ciMajor = if ($env:FL_WINENV_VS_MAJOR) { [int]$env:FL_WINENV_VS_MAJOR } else { 18 }
    $major = 0
    if ($vsVer -and ($vsVer -match '^(\d+)')) { $major = [int]$Matches[1] }
    if ($major -lt $ciMajor) {
        Write-Host ""
        Write-Host "WARNING: this guest builds with Visual Studio $major.x, but CI builds with $ciMajor.x."
        Write-Host "         Compiler-version-specific errors reported below may not reproduce in CI."
        Write-Host "         Re-run provisioning to install the matching Build Tools."
        Write-Host ""
    }
}

# ---- source ----
function Sync-Source {
    param([string]$Revision = "")

    if ($Source) { Write-Host "using caller-provided checkout: $Src"; return }

    if (-not (Test-Path (Join-Path $Src ".git"))) {
        Write-Host "first run: cloning $RepoUrl (this also gives an uploaded bundle its prerequisites)"
        Invoke-Checked git @("clone", $RepoUrl, $Src)
    }

    # The host uploads a bundle of merge-base(origin/main, HEAD)..HEAD. It is absent when the local
    # branch has nothing origin does not already have, in which case a plain fetch is enough.
    $bundle = "C:\fl\incoming\head.bundle"
    if (Test-Path $bundle) {
        Invoke-Checked git @("-C", $Src, "fetch", "--force", $bundle, "+refs/*:refs/winenv/*")
    } else {
        Invoke-Checked git @("-C", $Src, "fetch", "origin")
    }

    if ($Revision) {
        Invoke-Checked git @("-C", $Src, "checkout", "--detach", $Revision)
    }
    Invoke-Checked git @("-C", $Src, "submodule", "update", "--init", "--recursive")
    Write-Host "source at: $(& git -C $Src rev-parse HEAD)"
}

# ---- tier: ci ----
# 1:1 with the ci.yml windows-latest leg. debug-msvc inherits CMAKE_COMPILE_WARNING_AS_ERROR=ON,
# so /WX is on - which is the entire point of this environment: C4456 (shadowed local) and C4244
# (narrowing) are build failures here, and no GCC flag combination predicts them reliably.
function Invoke-TierCi {
    $env:CMAKE_TOOLCHAIN_FILE = "C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
    try {
        $cache = Join-Path $Src "build\debug-msvc\CMakeCache.txt"
        $configure = { Invoke-Checked cmake @("--preset", "debug-msvc", "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md") }
        # cmake/dependencies.cmake force-disables GNS when protobuf/OpenSSL are missing, so a broken
        # setup would otherwise SILENTLY build enet6-only and pass. CI asserts this; so do we.
        $gnsOn = { (Test-Path $cache) -and (Select-String -Path $cache -Pattern 'FL_ENABLE_GNS:BOOL=ON' -Quiet) }

        & $configure

        # A CACHED `NOTFOUND` IS NEVER RETRIED. find_package writes OPENSSL_INCLUDE_DIR-NOTFOUND into
        # the cache, and every later configure reuses it - so installing the missing dependency has
        # no effect and the tier keeps reporting a failure that has already been fixed. This env
        # keeps its build trees warm on purpose, which makes that stale-negative case the norm here
        # rather than the exception. Purge and reconfigure once before believing the assertion.
        if (-not (& $gnsOn)) {
            Write-Host "GNS reads as disabled - discarding the configure cache in case it is a stale NOTFOUND"
            Remove-Item $cache -Force -ErrorAction SilentlyContinue
            Remove-Item (Join-Path $Src "build\debug-msvc\CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
            & $configure
        }
        if (-not (& $gnsOn)) {
            throw "GNS was force-disabled at configure even after a clean reconfigure - OpenSSL or " +
                  "protobuf really is missing (check OPENSSL_ROOT_DIR and the vcpkg manifest)"
        }

        Invoke-Checked cmake @("--build", "--preset", "debug-msvc")
        Invoke-Checked ".\build\debug-msvc\game\fighters-legacy\fighters-legacy.exe" @("--version")
        Invoke-Checked ".\build\debug-msvc\server\fl-server\fl-server.exe" @("--version")
        Invoke-Checked ctest @("--preset", "debug-msvc", "--output-on-failure")
    } finally {
        Remove-Item Env:\CMAKE_TOOLCHAIN_FILE -ErrorAction SilentlyContinue
    }
}

# Both the smoke and runtime tiers build the lean enet6-only Release tree, which needs no vcpkg and
# no protobuf. Configuring is idempotent, so whichever tier runs first pays for it.
function Confirm-ReleaseConfigured {
    Remove-Item Env:\CMAKE_TOOLCHAIN_FILE -ErrorAction SilentlyContinue
    if (-not (Test-Path (Join-Path $Src "build\release-msvc\CMakeCache.txt"))) {
        Invoke-Checked cmake @("--preset", "release-msvc", "-DFL_ENABLE_GNS=OFF")
    }
}

# ---- tier: smoke ----
# 1:1 with scale-gate.yml's windows-smoke job - a bitrot guard for run_loadtest.ps1, not a
# measurement. 8 clients for 3 seconds; the script's own exit code is the gate.
function Invoke-TierSmoke {
    Confirm-ReleaseConfigured
    Invoke-Checked cmake @("--build", "--preset", "release-msvc", "--target", "fl-server", "bot_swarm")
    # pwsh, not powershell.exe: CI runs this script under PowerShell 7 (the default shell for a
    # `run:` step on a Windows runner), and 5.1 differs in enough defaults to be a different test.
    Invoke-Checked "pwsh" @("-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", ".\tools\bot_swarm\run_loadtest.ps1", "build\release-msvc", "8", "3", "weave")
}

# Echo whatever the game managed to say before it died. Windows startup failures in particular are
# reported only as an NTSTATUS exit code (0xC0000135 = a missing DLL, for instance), which names no
# file and points nowhere near the cause.
function Show-RuntimeLog([string]$OutFile, [string]$ErrFile) {
    foreach ($f in @(@{p=$OutFile; n="stdout"}, @{p=$ErrFile; n="stderr"})) {
        if (Test-Path $f.p) {
            $text = (Get-Content $f.p -Raw -ErrorAction SilentlyContinue)
            if ($text -and $text.Trim()) {
                Write-Host "--- game $($f.n) ---"
                ($text -split "`n" | Select-Object -Last 40) | ForEach-Object { Write-Host "  $($_.TrimEnd())" }
            } else {
                Write-Host "--- game $($f.n): empty ---"
            }
        }
    }
}

# ---- tier: runtime ----
# No CI equivalent: the hosted Windows legs never start the renderer. Renders the placeholder-mesh
# gallery and asserts a screenshot came back, which exercises window + swapchain creation, shader
# loading, the content path and the embedded fl-server handshake in one go.
function Invoke-TierRuntime {
    Confirm-ReleaseConfigured
    Invoke-Checked cmake @("--build", "--preset", "release-msvc",
                           "--target", "fighters-legacy", "fl-server")

    $png = Join-Path $OutDir "runtime-smoke.png"
    Remove-Item $png -ErrorAction SilentlyContinue

    if ($UseGpu) {
        # Leave the loader alone so it finds the installed GPU ICD.
        Remove-Item Env:\VK_DRIVER_FILES -ErrorAction SilentlyContinue
        Remove-Item Env:\VK_ICD_FILENAMES -ErrorAction SilentlyContinue
        Write-Host "renderer: passed-through GPU (system Vulkan ICD)"
    } else {
        if (-not (Test-Path $MesaIcd)) { throw "lavapipe ICD missing at $MesaIcd - re-run provisioning" }
        # VK_DRIVER_FILES is the modern loader variable; VK_ICD_FILENAMES is its deprecated alias,
        # set too so an older loader shipped beside the game still finds the software driver.
        $env:VK_DRIVER_FILES = $MesaIcd
        $env:VK_ICD_FILENAMES = $MesaIcd
        Write-Host "renderer: lavapipe (software) via $MesaIcd"
    }

    $game = Join-Path $Src "build\release-msvc\game\fighters-legacy\fighters-legacy.exe"
    # 120 frames rather than the 600-frame default: software rendering makes every frame expensive,
    # and the screenshot only has to prove the scene reached the swapchain.
    $gameArgs = @("--mission", "builtin:shape-gallery", "--screenshot", $png, "--screenshot-frames", "120")

    if ($ViaTask) {
        Invoke-RuntimeViaTask -Game $game -GameArgs $gameArgs
    } else {
        # CAPTURE THE GAME'S OUTPUT. Without this the tier can only report "exited with code N",
        # which for a startup failure says nothing at all - the first real failure here was a
        # missing DLL and the second was a bare exit 1, neither of which the harness could explain.
        # A check that cannot say why it failed costs more time than it saves.
        $stdout = Join-Path $OutDir "runtime-smoke.out.txt"
        $stderr = Join-Path $OutDir "runtime-smoke.err.txt"
        $p = Start-Process -FilePath $game -ArgumentList $gameArgs -WorkingDirectory $Src `
                           -NoNewWindow -PassThru `
                           -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (-not $p.WaitForExit(600000)) {
            $p.Kill()
            Show-RuntimeLog $stdout $stderr
            throw "runtime smoke: watchdog expired after 10 minutes"
        }
        if ($p.ExitCode -ne 0) { Show-RuntimeLog $stdout $stderr }
        if ($p.ExitCode -ne 0) { throw "game exited with code $($p.ExitCode)" }
    }

    if (-not (Test-Path $png)) {
        throw "runtime smoke: no screenshot at $png (see the README's session-0 troubleshooting)"
    }
    $size = (Get-Item $png).Length
    if ($size -lt 20KB) { throw "runtime smoke: screenshot is only $size bytes - nothing was drawn" }
    Write-Host "screenshot: $png ($([math]::Round($size / 1KB)) KB)"
}

# A WinRM shell has no interactive desktop, and a Vulkan swapchain needs a window. When direct
# execution cannot create one, this runs the game as a scheduled task in the logged-on console
# session instead (see the README for the autologon this depends on).
function Invoke-RuntimeViaTask {
    param([Parameter(Mandatory)][string]$Game, [Parameter(Mandatory)][string[]]$GameArgs)

    $taskName = "FL-RuntimeSmoke"
    $exitFile = Join-Path $OutDir "runtime-smoke.exit"
    Remove-Item $exitFile -ErrorAction SilentlyContinue

    # The wrapper records the exit code, which a scheduled task otherwise reports only as its own
    # last-run result - and does so asynchronously.
    $wrapper = Join-Path $OutDir "runtime-smoke-task.ps1"
    @"
`$ErrorActionPreference = 'Continue'
`$env:VK_DRIVER_FILES = '$($env:VK_DRIVER_FILES)'
`$env:VK_ICD_FILENAMES = '$($env:VK_ICD_FILENAMES)'
Set-Location '$Src'
& '$Game' $($GameArgs | ForEach-Object { "'$_'" })
`$LASTEXITCODE | Set-Content -Path '$exitFile'
"@ | Set-Content -Path $wrapper -Encoding UTF8

    $action = New-ScheduledTaskAction -Execute "pwsh.exe" `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$wrapper`""
    $principal = New-ScheduledTaskPrincipal -UserId "vagrant" -LogonType Interactive -RunLevel Highest
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $taskName

    $deadline = (Get-Date).AddMinutes(10)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        if (Test-Path $exitFile) { break }
    }
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue

    if (-not (Test-Path $exitFile)) { throw "runtime smoke (task): watchdog expired after 10 minutes" }
    $code = (Get-Content $exitFile -Raw).Trim()
    if ($code -ne "0" -and $code -ne "") { throw "runtime smoke (task): game exited with code $code" }
}

# ---- driver ----
Write-Section "windows-env: tiers [$($TierList -join ', ')]"
Write-Host "NOTE: this is a correctness environment. Do not read timings out of it."

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Enter-MsvcEnvironment
Sync-Source -Revision $Sha
Set-Location $Src

foreach ($tier in $TierList) {
    Write-Section "tier: $tier"
    $started = Get-Date
    try {
        switch ($tier) {
            "ci"      { Invoke-TierCi }
            "smoke"   { Invoke-TierSmoke }
            "runtime" { Invoke-TierRuntime }
            default   { throw "unknown tier '$tier' (expected ci, smoke or runtime)" }
        }
        $Results[$tier] = "PASS"
    } catch {
        Write-Host "FAILED: $($_.Exception.Message)"
        $Results[$tier] = "FAIL"
    }
    Write-Host "tier '$tier' finished in $([int]((Get-Date) - $started).TotalSeconds)s"
}

Write-Section "summary"
foreach ($k in $Results.Keys) { "{0,-10} {1}" -f $k, $Results[$k] | Write-Host }
if ($Results.Values -contains "FAIL") { exit 1 }
Write-Host "all selected tiers passed"
exit 0
