# ---------------------------------------------------------------------------
# build_mapping.ps1
#
# Builds the TextDeck input-mapping engine deliverables:
#   map_engine_selftest.exe  (unit tests, console)
#   map_mvp.exe              (6-scenario demo, console)
#
# Uses the same VS2022 Build Tools toolchain discovery as build_app.ps1, but is
# fully independent and does not touch the driver or require elevation.
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src  = Join-Path $root ''          # engine sources live directly under mapping/
$tests = Join-Path $root 'tests'
$out  = Join-Path $root 'bin'
$shared = Join-Path (Split-Path (Split-Path $root -Parent) -Parent) 'windows\shared'
if (-not (Test-Path $shared)) { throw "shared ABI header not found: $shared" }

New-Item -ItemType Directory -Force -Path $out | Out-Null

# ---- locate MSVC -----------------------------------------------------------
$msvcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$toolset = Get-ChildItem -Path $msvcRoot -Directory |
           Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } |
           Sort-Object { [version]($_.Name -replace '^(\d+\.\d+\.\d+).*', '$1') } |
           Select-Object -Last 1
if (-not $toolset) { throw "MSVC toolset not found" }
$msvc = $toolset.FullName

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = Get-ChildItem -Path (Join-Path $sdkRoot 'Include') -Directory |
          Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
          Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdkVer) { throw "Windows SDK not found" }
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

$cl = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { throw "cl.exe not found" }

$common = @('/nologo','/std:c++17','/EHsc','/W4','/O2','/utf-8','/DUNICODE','/D_UNICODE',('/I' + $shared))
$engineSources = @(
    (Join-Path $root 'map_core.cpp'),
    (Join-Path $root 'map_json.cpp'),
    (Join-Path $root 'map_engine.cpp'),
    (Join-Path $root 'map_queue.cpp'),
    (Join-Path $root 'map_raw_input.cpp'),
    (Join-Path $root 'map_adapters.cpp'),
    (Join-Path $root 'map_config.cpp')
)

function Invoke-Step([string]$exe, [string[]]$argv) {
    $out2 = & $exe @argv 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $out2 | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        throw "step failed (exit $code): $($argv -join ' ')"
    }
    return $code
}

Set-Location $out
# console subsystem: /SUBSYSTEM:CONSOLE, link ws2_32 for UDP + user32 for Raw Input/SendInput.
Invoke-Step $cl ($common + $engineSources + @((Join-Path $tests 'map_engine_selftest.cpp'),
    '/Fe:map_engine_selftest.exe','/Fd:map_engine_selftest.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib')) | Out-Null
Invoke-Step $cl ($common + $engineSources + @((Join-Path $root 'map_mvp_main.cpp'),
    '/Fe:map_mvp.exe','/Fd:map_mvp.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib')) | Out-Null
Invoke-Step $cl ($common + $engineSources + @((Join-Path $tests 'map_raw_input_test.cpp'),
    '/Fe:map_raw_input_test.exe','/Fd:map_raw_input_test.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib')) | Out-Null
Invoke-Step $cl ($common + $engineSources + @((Join-Path $tests 'map_v1_selftest.cpp'),
    '/Fe:map_v1_selftest.exe','/Fd:map_v1_selftest.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib')) | Out-Null
Invoke-Step $cl ($common + $engineSources + @((Join-Path $tests 'map_audit_test.cpp'),
    '/Fe:map_audit_test.exe','/Fd:map_audit_test.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib')) | Out-Null
Invoke-Step $cl ($common + $engineSources + @((Join-Path $root 'map_hid_e2e_main.cpp'),
    '/Fe:map_hid_e2e.exe','/Fd:map_hid_e2e.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib','gdi32.lib')) | Out-Null
Invoke-Step $cl ($common + $engineSources + @((Join-Path $root 'map_link_e2e_main.cpp'),
    '/Fe:map_link_e2e.exe','/Fd:map_link_e2e.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib','gdi32.lib')) | Out-Null
Invoke-Step $cl ($common + $engineSources + @((Join-Path $root 'map_app_main.cpp'),
    '/Fe:map_app.exe','/Fd:map_app.pdb','/link','/SUBSYSTEM:CONSOLE','ws2_32.lib','user32.lib')) | Out-Null

Write-Host "BUILD OK: selftest, mvp, raw_input_test, v1_selftest, audit_test, hid_e2e, link_e2e, map_app"
