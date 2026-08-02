# SPDX-License-Identifier: GPL-3.0-or-later
#
# provision.ps1 - installs the Windows toolchain in the validation VM (called by Vagrant, #1114).
# It does NOT run any checks; do that afterwards from the host so you control timing:
#
#   tools/windows-env/run-windows-check.sh
#
# Idempotent: every step checks for its own result first, so re-provisioning a live VM
# (`vagrant provision`) is cheap and safe.
#
# LICENSING. The guest is a Microsoft evaluation image (180 days - see the README for the rearm and
# rebuild paths) and the compiler is the Visual Studio Build Tools, whose licence covers building
# your own C++ with the installed workload. Nothing here is redistributed; every installer is
# fetched from its vendor at provision time.

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Version knobs. The Vulkan SDK is pinned to the same version the CI Windows leg installs, so the
# guest and the hosted runner agree about glslangValidator.
$VulkanVer = if ($env:FL_WINENV_VULKAN_VER) { $env:FL_WINENV_VULKAN_VER } else { "1.3.290.0" }
$MesaVer   = if ($env:FL_WINENV_MESA_VER)   { $env:FL_WINENV_MESA_VER }   else { "25.0.7" }
# Visual Studio channel: /17/ is VS 2022, /18/ is VS 2026. Override if Microsoft moves the alias.
$VsBootstrap = if ($env:FL_WINENV_VS_BOOTSTRAP) { $env:FL_WINENV_VS_BOOTSTRAP }
               else { "https://aka.ms/vs/18/release/vs_buildtools.exe" }
# NVIDIA driver for the GPU passthrough profile. No default: NVIDIA's download URLs are versioned
# and rot quickly, and a 404 mid-provision is worse than a clear instruction.
$NvidiaUrl = if ($env:FL_WINENV_NVIDIA_URL) { $env:FL_WINENV_NVIDIA_URL } else { "" }

$WorkRoot = "C:\fl"
$DlDir    = Join-Path $WorkRoot "dl"

function Write-Section([string]$Title) { Write-Host "=== $Title ===" }

function Get-RemoteFile([string]$Url, [string]$Dest) {
    if (Test-Path $Dest) { Write-Host "  cached: $Dest"; return }
    Write-Host "  downloading $Url"
    for ($try = 1; $try -le 3; $try++) {
        try { Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing; return }
        catch {
            if ($try -eq 3) { throw }
            Write-Host "  attempt $try failed, retrying: $($_.Exception.Message)"
            Start-Sleep -Seconds 10
        }
    }
}

Write-Section "workspace"
foreach ($d in @($WorkRoot, $DlDir, (Join-Path $WorkRoot "out"), (Join-Path $WorkRoot "incoming"),
                 (Join-Path $WorkRoot "windows-env"), (Join-Path $WorkRoot "vcpkg-bincache"))) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

# The qcow2 was created at FL_WINENV_DISK_GB, but the box's partition table still describes the
# original small disk - without this the C: drive fills during the first build.
Write-Section "grow system volume"
try {
    $max = (Get-PartitionSupportedSize -DriveLetter C).SizeMax
    $cur = (Get-Partition -DriveLetter C).Size
    if ($cur -lt ($max - 1GB)) {
        Resize-Partition -DriveLetter C -Size $max
        Write-Host "  C: grown to $([math]::Round($max / 1GB)) GB"
    } else {
        Write-Host "  C: already at maximum ($([math]::Round($cur / 1GB)) GB)"
    }
} catch {
    Write-Host "  WARNING: could not grow C: - $($_.Exception.Message)"
}

# Real-time scanning of a compiler's output directory is the single biggest avoidable cost in an
# MSVC build; the developer docs recommend the same exclusion for a bare-metal Windows checkout.
Write-Section "defender exclusions"
foreach ($p in @($WorkRoot, "C:\vcpkg", "C:\mesa")) {
    try { Add-MpPreference -ExclusionPath $p -ErrorAction Stop; Write-Host "  excluded $p" }
    catch { Write-Host "  skipped $p (Defender unavailable): $($_.Exception.Message)" }
}

Write-Section "chocolatey + base tools"
if (-not (Get-Command choco.exe -ErrorAction SilentlyContinue)) {
    Invoke-Expression ((New-Object Net.WebClient).DownloadString("https://community.chocolatey.org/install.ps1"))
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}
# CMake goes on the system PATH so a non-interactive WinRM shell finds it without a login profile.
choco install -y --no-progress git ninja 7zip
choco install -y --no-progress cmake --installargs 'ADD_CMAKE_TO_PATH=System'
# PowerShell 7. Not cosmetic: GitHub's Windows runners default `run:` steps to pwsh, so the tiers
# and run_loadtest.ps1 execute under pwsh in CI. Windows PowerShell 5.1 is a different shell with
# different defaults (notably file encoding), and mirroring a CI leg under the other one would make
# this environment disagree with the thing it exists to predict.
choco install -y --no-progress powershell-core
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine")

Write-Section "git configuration"
# MSVC object paths under a deep build tree exceed MAX_PATH, and a checkout that rewrites line
# endings would diverge from what the hosted runners compile.
git config --system core.longpaths true
git config --system core.autocrlf false
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" `
    -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force | Out-Null

Write-Section "visual studio build tools"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$haveVc = $false
if (Test-Path $vswhere) {
    $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($inst) { $haveVc = $true; Write-Host "  present: $inst" }
}
if (-not $haveVc) {
    $bootstrap = Join-Path $DlDir "vs_buildtools.exe"
    Get-RemoteFile $VsBootstrap $bootstrap
    Write-Host "  installing (this takes 30-60 minutes)"
    $p = Start-Process -FilePath $bootstrap -Wait -PassThru -ArgumentList @(
        "--quiet", "--wait", "--norestart", "--nocache",
        "--add", "Microsoft.VisualStudio.Workload.VCTools", "--includeRecommended")
    # 3010 is "installed, reboot pending" - a success the caller has to know about.
    if ($p.ExitCode -eq 3010) {
        Write-Host "  installed, REBOOT REQUIRED: run ``vagrant reload`` before the first check run"
    } elseif ($p.ExitCode -ne 0) {
        throw "VS Build Tools installer failed with exit code $($p.ExitCode)"
    }
}

Write-Section "vulkan sdk $VulkanVer"
if (Test-Path "C:\VulkanSDK\$VulkanVer\Bin\glslangValidator.exe") {
    Write-Host "  present"
} else {
    $sdk = Join-Path $DlDir "VulkanSDK-$VulkanVer-Installer.exe"
    Get-RemoteFile "https://sdk.lunarg.com/sdk/download/$VulkanVer/windows/VulkanSDK-$VulkanVer-Installer.exe" $sdk
    $p = Start-Process -FilePath $sdk -Wait -PassThru -ArgumentList @(
        "--root", "C:\VulkanSDK\$VulkanVer", "--accept-licenses", "--default-answer",
        "--confirm-command", "install")
    if ($p.ExitCode -ne 0) { throw "Vulkan SDK installer failed with exit code $($p.ExitCode)" }
    [Environment]::SetEnvironmentVariable("VULKAN_SDK", "C:\VulkanSDK\$VulkanVer", "Machine")
}

Write-Section "vcpkg"
# Manifest mode only: the repo-root vcpkg.json pins protobuf to the last pre-abseil 3.21.x, which
# is what GameNetworkingSockets v1.6.0 needs. Nothing is installed here - the toolchain file
# resolves the manifest at configure time, exactly as the CI Windows leg does.
if (-not (Test-Path "C:\vcpkg\vcpkg.exe")) {
    if (-not (Test-Path "C:\vcpkg")) { git clone https://github.com/microsoft/vcpkg C:\vcpkg }
    & C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
}
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "Machine")
[Environment]::SetEnvironmentVariable("VCPKG_DEFAULT_BINARY_CACHE",
    (Join-Path $WorkRoot "vcpkg-bincache"), "Machine")

Write-Section "mesa lavapipe $MesaVer"
# Software Vulkan, so the runtime tier can render without a GPU. Not registered system-wide - the
# check driver points VK_DRIVER_FILES at this ICD per run, which keeps the GPU profile a matter of
# not setting a variable rather than of uninstalling anything.
$icd = "C:\mesa\x64\lvp_icd.x86_64.json"
if (Test-Path $icd) {
    Write-Host "  present: $icd"
} else {
    $archive = Join-Path $DlDir "mesa3d-$MesaVer-release-msvc.7z"
    Get-RemoteFile "https://github.com/pal1000/mesa-dist-win/releases/download/$MesaVer/mesa3d-$MesaVer-release-msvc.7z" $archive
    & "$env:ProgramFiles\7-Zip\7z.exe" x $archive "-oC:\mesa" -y | Out-Null
    if (-not (Test-Path $icd)) {
        throw "mesa extracted but $icd is missing - check the mesa-dist-win layout for $MesaVer"
    }
}

Write-Section "nvidia driver (GPU passthrough profile)"
if (-not (Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
          Where-Object { $_.FriendlyName -match "NVIDIA" })) {
    Write-Host "  no NVIDIA display device present - skipping (lavapipe is the default renderer)"
} elseif (Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue) {
    Write-Host "  driver already installed"
} elseif (-not $NvidiaUrl) {
    Write-Host "  GPU present but FL_WINENV_NVIDIA_URL is unset."
    Write-Host "  Set it to a driver package URL and re-run ``vagrant provision``, or install the"
    Write-Host "  driver by hand in the guest. Until then the runtime tier must stay on lavapipe."
} else {
    $drv = Join-Path $DlDir "nvidia-driver.exe"
    Get-RemoteFile $NvidiaUrl $drv
    $p = Start-Process -FilePath $drv -Wait -PassThru -ArgumentList @("-s", "-noreboot", "-clean")
    if ($p.ExitCode -ne 0) { Write-Host "  WARNING: driver installer exit code $($p.ExitCode)" }
}

Write-Section "summary"
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine")
if (Test-Path $vswhere) {
    $vc = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    Write-Host "  msvc      : $vc"
}
Write-Host "  cmake     : $((cmake --version | Select-Object -First 1))"
Write-Host "  ninja     : $((ninja --version))"
Write-Host "  git       : $((git --version))"
Write-Host "  vulkan sdk: C:\VulkanSDK\$VulkanVer"
Write-Host "  vcpkg     : C:\vcpkg (binary cache $(Join-Path $WorkRoot 'vcpkg-bincache'))"
Write-Host "  lavapipe  : $icd"
Write-Host "  cpus      : $env:NUMBER_OF_PROCESSORS"
Write-Host ""
Write-Host "provisioned. Run checks from the host with: tools/windows-env/run-windows-check.sh"
