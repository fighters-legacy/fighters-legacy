# SPDX-License-Identifier: GPL-3.0-or-later
#
# ENet loopback latency measurement — Windows 10/11 (IOCP)
#
# No extra prerequisites required.
#   See docs/development.md — "Loopback latency analysis" section.
#
# Usage: .\tools\latency_analysis\measure_windows.ps1 [-BuildDir <path>]
#   BuildDir defaults to build\debug relative to repo root.
#
# Output: tools\latency_analysis\results\windows_<timestamp>.json
#   Run: python3 tools/latency_analysis/compare.py  to tabulate all results.
param(
    [string]$BuildDir = ""
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir  = $PSScriptRoot
$RepoRoot   = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "build\debug-msvc" }
$ResultsDir = Join-Path $ScriptDir "results"
$Timestamp  = (Get-Date -Format "yyyyMMddTHHmmssZ")
$Report     = Join-Path $ResultsDir "windows_$Timestamp.json"
$Port       = 4779
$BenchSamples = 600
$BenchRate    = 60

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

$FlServer = Join-Path $BuildDir "server\fl-server\fl-server.exe"
$NetChk   = Join-Path $BuildDir "tools\net_check.exe"
if (-not (Test-Path $FlServer)) {
    Write-Error "fl-server not found: $FlServer`nBuild first: cmake --build --preset debug-msvc"
}
if (-not (Test-Path $NetChk)) {
    Write-Error "net_check not found: $NetChk"
}

# System info
$OS  = (Get-CimInstance Win32_OperatingSystem).Caption
$CPU = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name

Write-Host "=== System: $OS ==="
Write-Host "=== CPU: $CPU ==="

# OS-level baseline — ICMP
Write-Host ""
Write-Host "--- ICMP ping baseline (100 packets) ---"
$PingRaw     = ping -n 100 127.0.0.1
$PingSummary = ($PingRaw | Select-String "Minimum|Maximum|Average") -join "; "
Write-Host $PingSummary

# Start fl-server
Write-Host ""
Write-Host "--- Starting fl-server on 127.0.0.1:$Port ---"
$SrvLog  = Join-Path $env:TEMP "fl_bench_$Timestamp.log"
$SrvProc = Start-Process -FilePath $FlServer `
    -ArgumentList "$Port 1 --bind 127.0.0.1 --admin-token benchtoken" `
    -PassThru -NoNewWindow -RedirectStandardOutput $SrvLog
Start-Sleep -Seconds 2

try {
    # ENet bench
    Write-Host ""
    Write-Host "--- net_check --bench $BenchSamples --bench-rate $BenchRate ---"
    $BenchOut = & $NetChk 127.0.0.1 $Port --bench $BenchSamples --bench-rate $BenchRate 2>&1
    Write-Host ($BenchOut -join "`n")

    # Write JSON report (ConvertTo-Json handles escaping)
    $ReportObj = [ordered]@{
        timestamp      = $Timestamp
        platform       = "windows"
        os             = $OS
        cpu            = $CPU
        bench_samples  = $BenchSamples
        bench_rate_hz  = $BenchRate
        icmp_summary   = $PingSummary
        bench_output   = ($BenchOut -join "`n")
    }
    $ReportObj | ConvertTo-Json -Depth 5 | Set-Content -Path $Report -Encoding UTF8

    Write-Host ""
    Write-Host "Report written to: $Report"
    Write-Host "Run: python3 tools/latency_analysis/compare.py  to tabulate all results."
}
finally {
    if (-not $SrvProc.HasExited) {
        $SrvProc.Kill()
    }
}
