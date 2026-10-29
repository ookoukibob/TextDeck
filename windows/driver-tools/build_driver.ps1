# ---------------------------------------------------------------------------
# build_driver.ps1 -- builds the TextDeck HID driver (x64, Debug).
#
# Requires Visual Studio Build Tools (or VS) with the MSVC toolset and the
# Windows Driver Kit. No admin rights are required to BUILD.
#
# The toolchain is DETECTED, not hardcoded: a specific compiler build number
# (e.g. 14.44.35207) is a local detail that changes with every VS servicing
# update, and a hardcoded path makes the script fail on every other machine.
# Pass -MSBuildPath / -ToolsetVersion to force a particular one.
#
# Signing is intentionally DISABLED (/p:SignMode=Off): the WDK default
# (TestSign) can auto-create and install a test-signing certificate, which is
# not done here without explicit confirmation. The produced .sys is therefore
# UNSIGNED and cannot load on a Secure Boot/HVCI/test-signing-off machine.
#
# Usage: .\build_driver.ps1 [-LogName driver-build-09] [-MSBuildPath <exe>] [-ToolsetVersion <ver>]
# Logs:  <repo>\build-logs\<LogName>.log         (msbuild file log)
#        <repo>\build-logs\<LogName>.log.console (console tail)
#        <repo>\build-logs\<LogName>.log.meta    (detected toolchain / exit / outputs)
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$LogName = 'driver-build',
    [string]$MSBuildPath = "",
    [string]$ToolsetVersion = ""
)

$ErrorActionPreference = 'Stop'

# --- repository-relative layout -------------------------------------------
# $PSScriptRoot is <repo>/windows/driver-tools, so the repository root is two
# levels up. Every path below is derived from it.
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$logDir   = Join-Path $repoRoot 'build-logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$sln = Join-Path $repoRoot 'windows\driver\TextDeckHidSolution.sln'
$log = Join-Path $logDir ($LogName + '.log')
$meta = "$log.meta"

if (-not (Test-Path $sln)) { throw "solution not found: $sln" }

# --- locate MSBuild -------------------------------------------------------
# NOTE: the 64-bit MSBuild host is required. The 32-bit
# MSBuild\Current\Bin\MSBuild.exe reports Processor_Architecture=x86, which
# makes InfToolPath resolve to the NuGet x86 tools directory that has no
# stampinf.exe (the build then fails at StampInf).
# An MSBuild installation only knows how to build a KMDF driver if the WDK
# platform toolset is registered with THAT installation. Detecting "the newest
# MSBuild" is therefore not enough: a newer VS without the WDK VSIX wins vswhere's
# -latest and then fails with "cannot open ntddk.h". Candidate order must prefer
# installations that actually carry the driver toolset.
function Test-HasWdkToolset([string]$msbuildExe) {
    # <VS>MSBuildCurrentBinamd64MSBuild.exe  ->  <VS>MSBuild
    $root = Split-Path (Split-Path (Split-Path $msbuildExe -Parent) -Parent) -Parent
    if (-not (Test-Path $root)) { return $false }
    $hits = Get-ChildItem -Path $root -Recurse -Directory -Filter 'WindowsKernelModeDriver10.0' -ErrorAction SilentlyContinue |
            Select-Object -First 1
    return [bool]$hits
}

function Find-MSBuild([string]$override) {
    if ($override) {
        if (-not (Test-Path $override)) { throw "MSBuildPath does not exist: $override" }
        if (-not (Test-HasWdkToolset $override)) {
            Write-Host ("WARNING: " + $override + " has no WindowsKernelModeDriver10.0 platform toolset; the build will fail to find ntddk.h")
        }
        return $override
    }

    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        # -all, not -latest: we rank the results ourselves by WDK presence.
        $candidates += @(& $vswhere -all -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' 2>$null)
    }
    foreach ($edition in @('BuildTools', 'Community', 'Professional', 'Enterprise')) {
        foreach ($year in @('2022', '18')) {
            $candidates += (Join-Path ${env:ProgramFiles(x86)} ("Microsoft Visual Studio\" + $year + "\" + $edition + "\MSBuild\Current\Bin\amd64\MSBuild.exe"))
        }
    }

    $candidates = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
    if (-not $candidates) {
        throw 'MSBuild (amd64) not found. Install Visual Studio Build Tools with the C++ workload, or pass -MSBuildPath.'
    }

    # First choice: an installation that carries the WDK platform toolset.
    foreach ($c in $candidates) {
        if (Test-HasWdkToolset $c) { return [string]$c }
    }

    Write-Host 'WARNING: no installed MSBuild carries the WindowsKernelModeDriver10.0 toolset.'
    Write-Host 'WARNING: install the WDK VSIX matching your Visual Studio, or pass -MSBuildPath.'
    return [string](@($candidates)[0])
}

# --- locate the MSVC toolset ----------------------------------------------
# Same reasoning: pick the newest installed toolset rather than pinning a
# build number that exists on one machine only.
function Find-Toolset([string]$msbuildPath, [string]$override) {
    $vcRoots = @()
    if ($msbuildPath -match '^(.*)\\MSBuild\\') { $vcRoots += (Join-Path $matches[1] 'VC\Tools\MSVC') }
    $vcRoots += (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC')
    $vcRoots += (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\Community\VC\Tools\MSVC')
    foreach ($root in ($vcRoots | Select-Object -Unique)) {
        if (-not (Test-Path $root)) { continue }
        $dirs = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^[0-9]+\.[0-9]+\.[0-9]+' }
        if ($override) { $dirs = $dirs | Where-Object { $_.Name -eq $override -or $_.Name -like "$override*" } }
        if (-not $dirs) { continue }
        $best = $dirs | Sort-Object { try { [version]($_.Name.Split("-")[0]) } catch { [version]"0.0.0" } } | Select-Object -Last 1
        $cl = Join-Path $best.FullName 'bin\HostX64\x64\cl.exe'
        if (Test-Path $cl) { return [string]$cl }
    }
    throw 'MSVC toolset not found. Install the C++ build tools, or pass -ToolsetVersion.'
}

$msb = Find-MSBuild $MSBuildPath
$cl  = Find-Toolset $msb $ToolsetVersion

# Inf2CatUseLocalTime=true -> Inf2Cat is invoked with /uselocaltime.
# WHY: stampinf writes DriverVer using the LOCAL clock, but Inf2Cat by default
# validates it against UTC. On any machine east of UTC the build then fails
# during the hours when the local date is already "tomorrow" in UTC, e.g.
#   22.9.7: DriverVer set to a date in the future (postdated DriverVer not allowed)
# /uselocaltime makes the verifier use the same clock stampinf used. This is a
# toolchain consistency setting, NOT a gate relaxation: Inf2Cat still performs
# every signability check.
$cmd = "$msb $sln /t:Build /p:Configuration=Debug /p:Platform=x64 /p:SignMode=Off /p:Inf2CatUseLocalTime=true"

# --- report the detected toolchain ----------------------------------------
$wdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$wdkVer = ''
if (Test-Path (Join-Path $wdkRoot 'Include')) {
    $wdkVer = (Get-ChildItem (Join-Path $wdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -match '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' } |
               Sort-Object { [version]$_.Name } | Select-Object -Last 1).Name
}
$vcVer = Split-Path (Split-Path (Split-Path (Split-Path (Split-Path $cl -Parent) -Parent) -Parent) -Parent) -Leaf

Write-Output ("toolchain: msbuild = " + $msb)
Write-Output ("toolchain: msbuild " + (Get-Item $msb).VersionInfo.FileVersion)
Write-Output ("toolchain: cl.exe  = " + $cl)
Write-Output ("toolchain: msvc    = " + $vcVer + " (" + (Get-Item $cl).VersionInfo.FileVersion + ")")
Write-Output ("toolchain: wdk/sdk = " + $wdkVer)

@(
    "time      : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "msbuild   : $msb"
    "msbuildVer: $((Get-Item $msb).VersionInfo.FileVersion)"
    "cl.exe    : $cl"
    "cl.exeVer : $((Get-Item $cl).VersionInfo.FileVersion)"
    "msvcVer   : $vcVer"
    "wdkSdkVer : $wdkVer"
    "command   : $cmd"
) | Set-Content -Path $meta -Encoding UTF8

& $msb $sln /t:Build /p:Configuration=Debug /p:Platform=x64 /p:SignMode=Off /p:Inf2CatUseLocalTime=true /v:n /nologo /fl "/flp:LogFile=$log;Verbosity=normal" 2>&1 | Tee-Object -FilePath ($log + ".console") | Select-Object -Last 40
$exit = $LASTEXITCODE

@("exit      : $exit") | Add-Content -Path $meta
@('outputs   :') | Add-Content -Path $meta
foreach ($dir in @(
        (Join-Path $repoRoot 'windows\driver\x64\Debug'),
        (Join-Path $repoRoot 'windows\driver\TextDeckHidDriver\x64\Debug'))) {
    if (Test-Path $dir) {
        Get-ChildItem $dir -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-30) } |
            ForEach-Object { "  $($_.FullName) ($($_.Length) bytes, $($_.LastWriteTime.ToString("HH:mm:ss")))" } |
            Add-Content -Path $meta
    }
}

Write-Output ("BUILD_EXIT=" + $exit)
exit $exit
