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
# Visual Studio bootstrapper channel.
#
# THE MAJOR VERSION MUST MATCH CI, or this environment reports failures that do not exist. GitHub's
# windows runner builds with `Microsoft Visual Studio\18\...\MSVC\14.51.x` (VS 2026). Provisioning
# VS 2022 instead (toolset 14.44) made the ci tier fail on `tests/test_http_admin.cpp` with C2017
# "illegal escape sequence" - a raw string containing \" inside a CHECK() macro, which 14.44's
# traditional preprocessor mis-tokenizes and 14.51 handles. Nothing was wrong with the code; a
# validation tool that invents failures is worse than no validation tool.
#
# `18/insiders` is the VS 2026 Build Tools bootstrapper (`insiders` resolves to the same download).
# NOTE aka.ms answers an UNKNOWN alias with a 200 OK Bing search page rather than a 404 - `18/pre`,
# `18/release` and `2026` are all dead - which is how a 190 KB HTML file once got saved as
# vs_buildtools.exe and failed half an hour later as "corrupted". Get-RemoteFile now catches that.
$VsBootstrap = if ($env:FL_WINENV_VS_BOOTSTRAP) { $env:FL_WINENV_VS_BOOTSTRAP }
               else { "https://aka.ms/vs/18/insiders/vs_buildtools.exe" }
# Reprovisioning must UPGRADE an older Visual Studio, not skip because some VS is present.
$VsMajorMin = if ($env:FL_WINENV_VS_MAJOR) { [int]$env:FL_WINENV_VS_MAJOR } else { 18 }
# NVIDIA driver for the GPU passthrough profile. No default: NVIDIA's download URLs are versioned
# and rot quickly, and a 404 mid-provision is worse than a clear instruction.
$NvidiaUrl = if ($env:FL_WINENV_NVIDIA_URL) { $env:FL_WINENV_NVIDIA_URL } else { "" }

$WorkRoot = "C:\fl"
$DlDir    = Join-Path $WorkRoot "dl"

function Write-Section([string]$Title) { Write-Host "=== $Title ===" }

# A download that returns 200 OK is not necessarily the file you asked for. aka.ms answers an
# unknown alias with a Bing search PAGE, and Invoke-WebRequest will happily write that HTML to
# whatever name you chose - producing a "vs_buildtools.exe" that fails 30 minutes later with
# "the file or directory is corrupted and unreadable", pointing nowhere near the actual mistake.
# So every download is checked against the magic bytes its extension implies, at the moment it
# lands. The check also runs over a CACHED file: without that, one bad download poisons the cache
# and every later run reuses it.
function Test-DownloadedFile([string]$Path, [string]$Magic) {
    if (-not (Test-Path $Path)) { return $false }
    $len = (Get-Item $Path).Length
    if ($len -lt 4) { return $false }
    $head = [IO.File]::ReadAllBytes($Path)[0..3]
    $ascii = -join ($head | ForEach-Object { [char]$_ })
    # An HTML body means a redirect to an error/search page, whatever the status code said.
    if ($ascii -match '^\s*(<!|<h|<H)') { return $false }
    if ($Magic -and -not $ascii.StartsWith($Magic)) { return $false }
    return $true
}

function Get-RemoteFile([string]$Url, [string]$Dest, [string]$Magic = "") {
    # The cache is keyed on the URL, not just the filename. Validating the SHAPE of a cached file is
    # not enough: when the Visual Studio channel changed, `vs_buildtools.exe` from the old URL was
    # still a perfectly valid PE, so it was served from cache and quietly installed the OLD compiler
    # while the log said it was installing the new one. A stale artefact that passes its own
    # integrity check is exactly the kind of plausible-but-wrong result worth designing against.
    $stamp = "$Dest.url"
    if (Test-Path $Dest) {
        $cachedUrl = if (Test-Path $stamp) { (Get-Content $stamp -Raw -ErrorAction SilentlyContinue).Trim() } else { "" }
        if ($cachedUrl -ne $Url) {
            Write-Host "  cached file at $Dest came from a different URL - discarding"
            Remove-Item $Dest, $stamp -Force -ErrorAction SilentlyContinue
        } elseif (Test-DownloadedFile $Dest $Magic) {
            Write-Host "  cached: $Dest"
            return
        } else {
            Write-Host "  cached file at $Dest is not a valid download - discarding"
            Remove-Item $Dest, $stamp -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host "  downloading $Url"
    for ($try = 1; $try -le 3; $try++) {
        try {
            Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
            if (Test-DownloadedFile $Dest $Magic) {
                Set-Content -Path $stamp -Value $Url -Encoding ASCII
                return
            }
            $size = if (Test-Path $Dest) { (Get-Item $Dest).Length } else { 0 }
            Remove-Item $Dest -Force -ErrorAction SilentlyContinue
            throw "the server returned $size bytes that are not a valid file (an HTML error or " +
                  "search page, most likely a dead URL): $Url"
        } catch {
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
# OpenSSL is a GameNetworkingSockets dependency, and it is NOT in the repo's vcpkg.json - CI never
# had to ask for it because GitHub's windows-latest image ships OpenSSL preinstalled. A bare Windows
# Server guest does not, so `cmake/dependencies.cmake` silently force-disables GNS and the ci tier
# builds enet6-only. (Found the hard way: the tier's own FL_ENABLE_GNS assertion caught it.)
choco install -y --no-progress openssl
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine")

# Point CMake's FindOpenSSL at it explicitly rather than trusting the install to land somewhere the
# module already probes - the choco package's directory carries its version and has moved before.
$sslRoot = Get-ChildItem "$env:ProgramFiles\OpenSSL*" -Directory -ErrorAction SilentlyContinue |
           Sort-Object Name -Descending | Select-Object -First 1
if ($sslRoot) {
    [Environment]::SetEnvironmentVariable("OPENSSL_ROOT_DIR", $sslRoot.FullName, "Machine")
    $env:OPENSSL_ROOT_DIR = $sslRoot.FullName
    Write-Host "  OPENSSL_ROOT_DIR = $($sslRoot.FullName)"
} else {
    Write-Host "  WARNING: OpenSSL install directory not found - the ci tier's GNS assertion will fail"
}

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
    $inst = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $ver  = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    $major = 0
    if ($ver -and ($ver -match '^(\d+)')) { $major = [int]$Matches[1] }
    if ($inst -and $major -ge $VsMajorMin) {
        $haveVc = $true
        Write-Host "  present: $inst (version $ver)"
    } elseif ($inst) {
        # Present but too old to match CI - upgrade rather than silently keep predicting with the
        # wrong compiler. The bootstrapper installs the newer product side by side.
        Write-Host "  found Visual Studio $ver, but CI builds with $VsMajorMin.x - installing the newer Build Tools"
    }
}
if (-not $haveVc) {
    $bootstrap = Join-Path $DlDir "vs_buildtools.exe"
    Get-RemoteFile $VsBootstrap $bootstrap "MZ"
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
    Get-RemoteFile "https://sdk.lunarg.com/sdk/download/$VulkanVer/windows/VulkanSDK-$VulkanVer-Installer.exe" $sdk "MZ"
    $p = Start-Process -FilePath $sdk -Wait -PassThru -ArgumentList @(
        "--root", "C:\VulkanSDK\$VulkanVer", "--accept-licenses", "--default-answer",
        "--confirm-command", "install")
    if ($p.ExitCode -ne 0) { throw "Vulkan SDK installer failed with exit code $($p.ExitCode)" }
    [Environment]::SetEnvironmentVariable("VULKAN_SDK", "C:\VulkanSDK\$VulkanVer", "Machine")
}

Write-Section "vulkan loader (runtime)"
# The SDK ships headers, libs and tools but NOT the loader, and `vulkan-1.dll` normally arrives with
# a GPU driver - which a headless guest has none of. Without it the game links fine and then dies at
# startup with 0xC0000135 (STATUS_DLL_NOT_FOUND), nowhere near anything Vulkan-shaped. lavapipe
# supplies the ICD and the driver (`vulkan_lvp.dll`); this supplies the loader that finds them.
if (Test-Path "$env:SystemRoot\System32\vulkan-1.dll") {
    Write-Host "  present"
} else {
    $rt = Join-Path $DlDir "VulkanRT-$VulkanVer-Installer.exe"
    Get-RemoteFile "https://sdk.lunarg.com/sdk/download/$VulkanVer/windows/VulkanRT-$VulkanVer-Installer.exe" $rt "MZ"
    $p = Start-Process -FilePath $rt -Wait -PassThru -ArgumentList @("/S")
    if ($p.ExitCode -ne 0) { Write-Host "  WARNING: Vulkan runtime installer exit code $($p.ExitCode)" }
    if (-not (Test-Path "$env:SystemRoot\System32\vulkan-1.dll")) {
        throw "Vulkan loader still absent after installing the runtime - the runtime tier cannot start the game"
    }
    Write-Host "  installed $env:SystemRoot\System32\vulkan-1.dll"
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
    Get-RemoteFile "https://github.com/pal1000/mesa-dist-win/releases/download/$MesaVer/mesa3d-$MesaVer-release-msvc.7z" $archive "7z"
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
    Get-RemoteFile $NvidiaUrl $drv "MZ"
    $p = Start-Process -FilePath $drv -Wait -PassThru -ArgumentList @("-s", "-noreboot", "-clean")
    if ($p.ExitCode -ne 0) { Write-Host "  WARNING: driver installer exit code $($p.ExitCode)" }
}

Write-Section "summary"
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine")
if (Test-Path $vswhere) {
    $vc = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
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
