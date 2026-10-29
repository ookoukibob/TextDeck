# ---------------------------------------------------------------------------
# build_state_test.ps1
#
# Builds and runs the host-side unit tests for the firmware HidTyper
# non-blocking state machine. The PRODUCTION source
# (../esp32_hid_text_device/hid_typer.cpp) is compiled unchanged with
# HID_TYPER_HOST_TEST, which swaps Arduino/USBHIDKeyboard for the fake HAL in
# host_test_hal.h/cpp. Nothing is re-implemented for the test, so the state
# machine under test is exactly the code that runs on the ESP32.
#
# The toolchain is located by directory probe and the compiler environment is
# set explicitly: vcvarsall.bat shells out to reg.exe/vswhere, which is not
# available (and is blocked) in this environment.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File build_state_test.ps1
# Exit:   0 = all checks passed
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# The firmware sources live one level up: $here is <sketch>/test and the
# production sources sit directly in <sketch>/.
$fw   = Split-Path -Parent $here

$msvcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$toolset = Get-ChildItem -Path $msvcRoot -Directory |
           Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } |
           Sort-Object { [version]($_.Name -replace '^(\d+\.\d+\.\d+).*', '$1') } |
           Select-Object -Last 1
if (-not $toolset) { throw "MSVC toolset not found under $msvcRoot" }
$msvc = $toolset.FullName

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = Get-ChildItem -Path (Join-Path $sdkRoot 'Include') -Directory |
          Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
          Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdkVer) { throw "Windows SDK not found under $sdkRoot\Include" }
$sdkName = $sdkVer.Name

$env:INCLUDE = (@(
    (Join-Path $msvc 'include'),
    (Join-Path $sdkRoot ('Include\' + $sdkName + '\ucrt')),
    (Join-Path $sdkRoot ('Include\' + $sdkName + '\um')),
    (Join-Path $sdkRoot ('Include\' + $sdkName + '\shared'))
) -join ';')
$env:LIB = (@(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $sdkRoot ('Lib\' + $sdkName + '\ucrt\x64')),
    (Join-Path $sdkRoot ('Lib\' + $sdkName + '\um\x64'))
) -join ';')
$env:PATH = (@(
    (Join-Path $msvc 'bin\Hostx64\x64'),
    (Join-Path $sdkRoot ('bin\' + $sdkName + '\x64')),
    $env:PATH
) -join ';')

$cl = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$out = Join-Path $here 'state_test.exe'

Push-Location $here
try {
    & $cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 /DHID_TYPER_HOST_TEST `
        ("/I" + $here) ("/I" + $fw) `
        (Join-Path $here 'state_test.cpp') `
        (Join-Path $here 'host_test_hal.cpp') `
        (Join-Path $fw 'hid_typer.cpp') `
        /Fe:$out /Fd:(Join-Path $here 'state_test.pdb')
    if ($LASTEXITCODE -ne 0) { throw "compile failed (exit $LASTEXITCODE)" }

    & $out
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}
Write-Output ("STATE_TEST_EXIT=" + $rc)
exit $rc
