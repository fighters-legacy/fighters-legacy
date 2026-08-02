# SPDX-License-Identifier: GPL-3.0-or-later
#
# setup-runner.ps1 - register a GitHub Actions self-hosted Windows runner (#1114). Runs INSIDE the
# validation VM (the Vagrantfile guest) or on any Windows box with the toolchain provisioned. The
# Windows counterpart of tools/bot_swarm/reference-env/setup-self-hosted-runner.sh, and the
# version-controlled form of the runbook in README.md -> "Self-hosted Windows runner". Idempotent.
#
# The runner is NON-EPHEMERAL, so a warm build tree survives between jobs where the cleanup hook
# allows it; the hook reclaims the workspace so non-ephemeral state cannot accumulate indefinitely.
#
# SECURITY (public repo + self-hosted). Fork-PR code must never execute here. The layers:
#   * windows-env.yml is `workflow_dispatch`-only (write-access-gated) behind a repository guard and
#     an operator on-switch (the FL_WINDOWS_RUNNER_READY repo variable). No pull_request path exists.
#   * The VM is the sandbox - a compromise's blast radius is a disposable guest, and the guest holds
#     no host filesystem mount (source arrives by upload, not by synced folder).
#   * The service runs as a dedicated NON-ADMINISTRATOR local account. Windows has no equivalent of
#     the systemd hardening drop-in the Linux reference runner installs, so the unprivileged account
#     plus the VM boundary plus the trigger guard are the compensating controls. Do not run this on
#     a box you would mind losing.
#
# Usage (elevated PowerShell):
#     $env:FL_RUNNER_TOKEN = "<registration-token>"; .\setup-runner.ps1
#
#   FL_RUNNER_TOKEN     required. Short-lived registration token from the repo:
#                       Settings -> Actions -> Runners -> New self-hosted runner. Never committed.
#   FL_RUNNER_VERSION   optional. actions/runner version (default: latest via the API, falling back
#                       to the pinned FallbackVersion below).
#   FL_RUNNER_SHA256    optional. If set, the downloaded zip is verified against it (the value is
#                       shown on the same "New self-hosted runner" page).
#   FL_RUNNER_URL       optional. Repo URL (default: the canonical fighters-legacy repo).
#   FL_RUNNER_NAME      optional. Runner name (default: fl-windows-vm).

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---- config ----
$RunnerUser  = "flrunner"
$RunnerHome  = "C:\actions-runner"
$RunnerUrl   = if ($env:FL_RUNNER_URL)  { $env:FL_RUNNER_URL }  else { "https://github.com/fighters-legacy/fighters-legacy" }
$RunnerName  = if ($env:FL_RUNNER_NAME) { $env:FL_RUNNER_NAME } else { "fl-windows-vm" }
# Custom label only - the runner auto-applies self-hosted,windows,x64; together they satisfy the
# workflow's `runs-on: [self-hosted, windows, x64, fl-windows]`.
$RunnerLabels = "fl-windows"
$FallbackVersion = "2.328.0"

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "run elevated (this creates a local user and installs a service)"
}
if (-not $env:FL_RUNNER_TOKEN) {
    throw "FL_RUNNER_TOKEN is required (repo Settings -> Actions -> Runners -> New self-hosted runner)"
}

if (Get-Service -Name "actions.runner.*" -ErrorAction SilentlyContinue) {
    Write-Host "=== a runner service is already installed; remove it first to re-register ==="
    Get-Service -Name "actions.runner.*" | Format-Table -AutoSize
    Write-Host "  cd $RunnerHome; .\svc.sh uninstall  (or: .\config.cmd remove --token <removal-token>)"
    exit 0
}

# ---- dedicated unprivileged runner account ----
# The password exists only for the duration of this script and is handed straight to config.cmd,
# which stores it in the service's LSA secret. It is never written to disk or echoed.
Write-Host "=== creating unprivileged local account '$RunnerUser' ==="
$bytes = [byte[]]::new(24)
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$plainPw = [Convert]::ToBase64String($bytes)
$securePw = ConvertTo-SecureString $plainPw -AsPlainText -Force

if (Get-LocalUser -Name $RunnerUser -ErrorAction SilentlyContinue) {
    Set-LocalUser -Name $RunnerUser -Password $securePw -PasswordNeverExpires $true
} else {
    New-LocalUser -Name $RunnerUser -Password $securePw -PasswordNeverExpires `
        -Description "GitHub Actions runner service account (fighters-legacy windows-env)" | Out-Null
}
# Deliberately NOT added to Administrators. If a job needs elevation, that is a review conversation,
# not a quiet membership change here.

# ---- download the runner ----
$version = $env:FL_RUNNER_VERSION
if (-not $version) {
    try {
        $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/actions/runner/releases/latest" `
            -Headers @{ "User-Agent" = "fl-windows-env" } -UseBasicParsing
        $version = $rel.tag_name.TrimStart("v")
    } catch {
        Write-Host "  GitHub API lookup failed, using pinned fallback $FallbackVersion"
        $version = $FallbackVersion
    }
}
Write-Host "=== installing actions/runner $version to $RunnerHome ==="
New-Item -ItemType Directory -Force -Path $RunnerHome | Out-Null
$zip = Join-Path $env:TEMP "actions-runner-win-x64-$version.zip"
if (-not (Test-Path $zip)) {
    Invoke-WebRequest -UseBasicParsing -OutFile $zip `
        -Uri "https://github.com/actions/runner/releases/download/v$version/actions-runner-win-x64-$version.zip"
}
if ($env:FL_RUNNER_SHA256) {
    $actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
    if ($actual -ne $env:FL_RUNNER_SHA256.ToUpper()) {
        throw "runner zip checksum mismatch: expected $($env:FL_RUNNER_SHA256), got $actual"
    }
    Write-Host "  checksum verified"
}
Expand-Archive -Path $zip -DestinationPath $RunnerHome -Force

# The service account owns the runner directory; it needs to write _work, _diag and .env there.
$acl = Get-Acl $RunnerHome
$acl.SetAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
    $RunnerUser, "Modify", "ContainerInherit,ObjectInherit", "None", "Allow")))
Set-Acl -Path $RunnerHome -AclObject $acl

# ---- per-job workspace cleanup hook ----
# Wired before configuring so the very first job already gets it. Mirrors the Linux runner's
# ACTIONS_RUNNER_HOOK_JOB_STARTED/COMPLETED pair.
Write-Host "=== installing the per-job cleanup hook ==="
Copy-Item -Path (Join-Path $ScriptDir "runner-cleanup.ps1") -Destination $RunnerHome -Force
$hook = Join-Path $RunnerHome "runner-cleanup.ps1"
$envFile = Join-Path $RunnerHome ".env"
$existing = if (Test-Path $envFile) {
    Get-Content $envFile | Where-Object { $_ -notmatch "^ACTIONS_RUNNER_HOOK_JOB_" }
} else { @() }
($existing + @(
    "ACTIONS_RUNNER_HOOK_JOB_STARTED=$hook",
    "ACTIONS_RUNNER_HOOK_JOB_COMPLETED=$hook"
)) | Set-Content -Path $envFile -Encoding ASCII

# ---- configure + install the service ----
Write-Host "=== configuring runner '$RunnerName' (labels: $RunnerLabels) ==="
Push-Location $RunnerHome
try {
    & .\config.cmd --url $RunnerUrl --token $env:FL_RUNNER_TOKEN --labels $RunnerLabels `
        --name $RunnerName --unattended --replace `
        --runasservice --windowslogonaccount ".\$RunnerUser" --windowslogonpassword $plainPw
    if ($LASTEXITCODE -ne 0) { throw "config.cmd failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
    $plainPw = $null
    [GC]::Collect()
}

# Real-time scanning of a checkout plus two MSVC build trees costs more than it protects here; the
# guest is disposable and the workspace is wiped between jobs.
try {
    Add-MpPreference -ExclusionPath (Join-Path $RunnerHome "_work") -ErrorAction Stop
} catch {
    Write-Host "  note: could not add a Defender exclusion for _work - $($_.Exception.Message)"
}

Write-Host ""
Write-Host "=== done ==="
Write-Host "  status   : Get-Service 'actions.runner.*'"
Write-Host "  enable CI: set the repo variable FL_WINDOWS_RUNNER_READY = true"
Write-Host "             (Settings -> Actions -> Variables). windows-env.yml stays skipped until then."
Write-Host "  dispatch : gh workflow run windows-env.yml -f tiers=ci,smoke"
Write-Host "  remove   : cd $RunnerHome; .\config.cmd remove --token <removal-token>"
