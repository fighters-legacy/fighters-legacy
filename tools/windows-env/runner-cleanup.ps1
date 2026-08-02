# SPDX-License-Identifier: GPL-3.0-or-later
#
# runner-cleanup.ps1 - per-job workspace cleanup hook for the self-hosted Windows runner (#1114).
# The Windows counterpart of tools/bot_swarm/reference-env/runner-job-cleanup.sh; installed by
# setup-runner.ps1 as both ACTIONS_RUNNER_HOOK_JOB_STARTED and ..._JOB_COMPLETED.
#
# The runner is non-ephemeral, so without this a Release + Debug MSVC build tree accumulates in the
# workspace until the disk fills.
#
# IT MUST NEVER FAIL A JOB. A hook that exits non-zero fails the job it was supposed to tidy up
# after, so every step is wrapped and the script always exits 0 - the same reasoning behind the
# Linux hook's `set -uo pipefail` (deliberately no `-e`) with `|| true` on every action.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$ws = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE }
      elseif ($env:RUNNER_WORKSPACE) { $env:RUNNER_WORKSPACE }
      else { Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "_work" }

Write-Host "runner-cleanup: workspace $ws"

# A crashed job leaves fl-server, bot_swarm or the game running. On Windows a live process holds an
# open handle to its own binary, so the cleanup below would fail on exactly the files that matter -
# unlike Linux, where an unlinked-but-open file is not an obstacle.
foreach ($name in @("fl-server", "bot_swarm", "fighters-legacy", "net_check")) {
    try {
        Get-Process -Name $name -ErrorAction SilentlyContinue |
            ForEach-Object {
                Write-Host "  killing stray $name (pid $($_.Id))"
                Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            }
    } catch {
        Write-Host "  could not check for stray $name (ignored): $($_.Exception.Message)"
    }
}
Start-Sleep -Milliseconds 500

try {
    if (-not (Test-Path $ws)) {
        Write-Host "  workspace does not exist yet - nothing to clean"
    } elseif (Test-Path (Join-Path $ws ".git")) {
        # A git tree cleans faster and more precisely than a recursive delete, and keeps the object
        # store so the next checkout is incremental.
        Push-Location $ws
        try {
            & git clean -xffd 2>&1 | Out-Null
            & git reset --hard 2>&1 | Out-Null
            Write-Host "  git clean + reset complete"
        } finally { Pop-Location }
    } else {
        Get-ChildItem -Path $ws -Force -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item -Path $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
        Write-Host "  workspace contents removed"
    }
} catch {
    Write-Host "  cleanup incomplete (ignored): $($_.Exception.Message)"
}

exit 0
