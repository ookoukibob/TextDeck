# ---------------------------------------------------------------------------
# build_app.ps1
#
# Builds VirtualHIDTyper.exe (GUI, with embedded manifest) and map_selftest.exe
# (console) with the VS2022 Build Tools (x64). No admin rights required.
# Logs: ..\logs\app-build.log
#
# The EXE embeds VirtualHIDTyper.manifest (asInvoker + comctl32 v6 + DPI
# awareness) through VirtualHIDTyper.rc. The ESP32 USB HID backend needs no
# kernel driver and no elevation, so the whole app starts as a normal user.
# The legacy Virtual HID (kernel driver) mode still reports its own
# access-denied error at start time when the driver is unreachable; it never
# forces a UAC prompt for the ESP32 / software paths.
#
# The toolchain is located by DIRECTORY PROBE and the compiler environment is
# set here explicitly. We deliberately do not call vcvarsall.bat: it shells out
# to reg.exe/vswhere, which is unavailable (and blocked) in this environment.
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src  = Join-Path $root 'src'
$out  = Join-Path $root 'bin'
$log  = Join-Path (Join-Path $root '..') 'build-logs\app-build.log'

New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null

# ---- locate MSVC (highest installed 14.xx toolset) -------------------------
$msvcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$toolset = Get-ChildItem -Path $msvcRoot -Directory |
           Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } |
           Sort-Object { [version]($_.Name -replace '^(\d+\.\d+\.\d+).*', '$1') } |
           Select-Object -Last 1
if (-not $toolset) { throw "MSVC toolset not found under $msvcRoot" }
$msvc = $toolset.FullName

# ---- locate Windows 10/11 SDK (highest installed) --------------------------
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = Get-ChildItem -Path (Join-Path $sdkRoot 'Include') -Directory |
          Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
          Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdkVer) { throw "Windows SDK not found under $sdkRoot\Include" }
$sdk = $sdkRoot
$sdkVerName = $sdkVer.Name

$env:INCLUDE = (@(
    (Join-Path $msvc 'include'),
    (Join-Path $sdk ('Include\' + $sdkVerName + '\ucrt')),
    (Join-Path $sdk ('Include\' + $sdkVerName + '\um')),
    (Join-Path $sdk ('Include\' + $sdkVerName + '\shared')),
    (Join-Path $sdk ('Include\' + $sdkVerName + '\winrt'))
) -join ';')
$env:LIB = (@(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $sdk ('Lib\' + $sdkVerName + '\ucrt\x64')),
    (Join-Path $sdk ('Lib\' + $sdkVerName + '\um\x64'))
) -join ';')
$env:PATH = (@(
    (Join-Path $msvc 'bin\Hostx64\x64'),
    (Join-Path $sdkRoot ('bin\' + $sdkVerName + '\x64')),
    $env:PATH
) -join ';')

$cl  = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$rc  = Join-Path $sdkRoot ('bin\' + $sdkVerName + '\x64\rc.exe')
if (-not (Test-Path $cl)) { throw "cl.exe not found: $cl" }
if (-not (Test-Path $rc)) { throw "rc.exe not found: $rc" }

# windows/shared holds the driver<->client ABI header. Both the driver and the
# application compile against the SAME file, so a struct change cannot land on
# one side only.
$shared = Join-Path (Split-Path (Split-Path $root -Parent) -Parent) 'windows\shared'
if (-not (Test-Path $shared)) { throw "shared ABI header directory not found: $shared" }
$common = @('/nologo','/std:c++17','/EHsc','/W4','/O2','/utf-8','/Zi','/DUNICODE','/D_UNICODE',('/I' + $shared))
$res    = Join-Path $out 'VirtualHIDTyper.res'

$appSources = @(
    (Join-Path $src 'VirtualHIDTyper.cpp'),
    (Join-Path $src 'hid_mapping.cpp'),
    (Join-Path $src 'sw_input.cpp'),
    (Join-Path $src 'esp32_protocol.cpp'),
    (Join-Path $src 'esp32_link.cpp')
)
$selfSources = @(
    (Join-Path $src 'hid_mapping.cpp'),
    (Join-Path $src 'sw_input.cpp'),
    (Join-Path $src 'esp32_protocol.cpp'),
    (Join-Path $src 'map_selftest.cpp')
)

Set-Location $out
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# toolset=$($toolset.Name) sdk=$sdkVerName")
$lines.Add("# cl=$cl")
$lines.Add("# rc=$rc")

function Invoke-Step([string]$exe, [string[]]$argv) {
    $lines.Add("")
    $lines.Add("> $exe $($argv -join ' ')")
    $output = & $exe @argv 2>&1
    $code = $LASTEXITCODE
    foreach ($l in $output) { $lines.Add([string]$l) }
    if ($code -ne 0) {
        $lines.Add("EXITCODE=$code")
        $lines | Out-File -FilePath $log -Encoding utf8
        Get-Content $log -Tail 40
        throw "step failed: $exe (exit $code)"
    }
    return $code
}

Invoke-Step $rc (@('/nologo',
    ('/I' + (Join-Path $sdk ('Include\' + $sdkVerName + '\um'))),
    ('/I' + (Join-Path $sdk ('Include\' + $sdkVerName + '\shared'))),
    ('/I' + (Join-Path $sdk ('Include\' + $sdkVerName + '\ucrt'))),
    '/fo',$res,(Join-Path $src 'VirtualHIDTyper.rc'))) | Out-Null
Invoke-Step $cl ($common + $selfSources + @('/Fe:map_selftest.exe','/Fd:map_selftest.pdb')) | Out-Null
Invoke-Step $cl ($common + $appSources + @($res,'/Fe:VirtualHIDTyper.exe','/Fd:VirtualHIDTyper.pdb','/link','/SUBSYSTEM:WINDOWS','/MANIFEST:NO')) | Out-Null

$lines.Add("")
$lines.Add("EXITCODE=0")
$lines | Out-File -FilePath $log -Encoding utf8
Get-Content $log -Tail 12
