# ---------------------------------------------------------------------------
# build_logic_test.ps1
#
# Builds and runs the host-side driver logic test.
#
# The function under test (TextDeckBuildReport) lives in the driver's Device.c.
# Rather than copying it into the test -- which would let the copy drift from
# the shipping code -- this script EXTRACTS the function body verbatim from
# Device.c and compiles that. If the driver changes, the test changes with it.
#
# Exit: 0 = all checks passed
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path (Split-Path $here -Parent) -Parent
$device = Join-Path $repo 'windows\driver\TextDeckHidDriver\Device.c'

if (-not (Test-Path $device)) { throw "driver source not found: $device" }

# ---- extract TextDeckBuildReport() verbatim -------------------------------
$src = [System.IO.File]::ReadAllText($device)

# Extract one C function body verbatim, from its definition to its matching brace.
function Get-FunctionBody([string]$text, [string]$marker) {
    $start = $text.IndexOf($marker)
    if ($start -lt 0) { throw "$marker not found in Device.c" }
    $brace = $text.IndexOf('{', $start)
    if ($brace -lt 0) { throw "opening brace not found for $marker" }
    $depth = 0
    for ($i = $brace; $i -lt $text.Length; $i++) {
        $c = $text[$i]
        if ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') {
            $depth--
            if ($depth -eq 0) { return $text.Substring($brace, $i - $brace + 1) }
        }
    }
    throw "closing brace not found for $marker"
}

$fnBody = Get-FunctionBody $src 'TextDeckBuildReport('
$ledBody = Get-FunctionBody $src 'TextDeckDecodeLedReport('

# Re-declare the signatures in host-compatible terms.
$extracted = @(
    '// ---- BEGIN extracted from Device.c (do not edit here) ----'
    'NTSTATUS TextDeckBuildReport(const TEXTDECK_KEY_EVENT* Event, UCHAR* Report)'
    $fnBody
    'NTSTATUS TextDeckDecodeLedReport(const UCHAR* Buffer, ULONG Length, ULONG* Leds)'
    $ledBody
    '// ---- END extracted from Device.c ----'
    ''
) -join "`r`n"

$gen = Join-Path $here 'driver_logic_extracted.inc'
[System.IO.File]::WriteAllText($gen, $extracted, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ('extracted ' + $fnBody.Length + ' + ' + $ledBody.Length + ' chars from Device.c')

# ---- locate the toolchain by directory probe -----------------------------
$msvcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$toolset = Get-ChildItem -Path $msvcRoot -Directory -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } |
           Sort-Object { [version]($_.Name -replace '^(\d+\.\d+\.\d+).*', '$1') } |
           Select-Object -Last 1
if (-not $toolset) { throw "MSVC toolset not found under $msvcRoot" }
$msvc = $toolset.FullName

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = Get-ChildItem -Path (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
          Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdkVer) { throw 'Windows SDK not found' }
$sdk = $sdkVer.Name

$env:INCLUDE = (@(
    (Join-Path $msvc 'include'),
    (Join-Path $sdkRoot ('Include\' + $sdk + '\ucrt')),
    (Join-Path $sdkRoot ('Include\' + $sdk + '\um')),
    (Join-Path $sdkRoot ('Include\' + $sdk + '\shared'))
) -join ';')
$env:LIB = (@(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $sdkRoot ('Lib\' + $sdk + '\ucrt\x64')),
    (Join-Path $sdkRoot ('Lib\' + $sdk + '\um\x64'))
) -join ';')
$env:PATH = (@(
    (Join-Path $msvc 'bin\Hostx64\x64'),
    (Join-Path $sdkRoot ('bin\' + $sdk + '\x64')),
    $env:PATH
) -join ';')

$cl = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$out = Join-Path $here 'driver_logic_test.exe'

Push-Location $here
try {
    & $cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 `
        (Join-Path $here 'driver_logic_test.cpp') `
        "/Fe:$out" "/Fd:$((Join-Path $here 'driver_logic_test.pdb'))"
    if ($LASTEXITCODE -ne 0) { throw "compile failed (exit $LASTEXITCODE)" }
    & $out
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}
Write-Output ('LOGIC_TEST_EXIT=' + $rc)
exit $rc