# Regression test for the settings.ini section/BOM defect.
#
# Measured behaviour of the Win32 profile API on this platform (see the comment
# block above NormalizeIniSections in src/VirtualHIDTyper.cpp):
#   * a UTF-8 BOM is NOT stripped, so the first section header is not
#     recognised; a BOM file holding one [Main] yields NO values at all, and a
#     BOM file holding two exposes only the second one;
#   * without a BOM the first matching section is the one that resolves.
#
# The app must repair such a file once, keep every value the user could observe,
# leave a clean file alone, and never truncate the only profile it has.
#
# The smoke process is treated as a FAILURE unless all of these hold:
#   * it exits (not killed) within the timeout;
#   * it exits with code 0;
#   * its log contains the smoke completion marker.
# A hang, a crash or a silent no-op must never look like a pass.
#
# Usage: powershell -File test_ini_normalize.ps1 [-ExePath <path>]
param(
    [string]$ExePath = "",
    [int]$SmokeTimeoutMs = 30000
)

$ErrorActionPreference = "Stop"
$NL = [string][char]13 + [string][char]10
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
if (-not $ExePath) {
    $ExePath = Join-Path $repo "windows\VirtualHIDTyper\bin\VirtualHIDTyper.exe"
}

$script:pass = 0
$script:fail = 0
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) {
        $script:pass++
        Write-Output ("  PASS  " + $name)
    } else {
        $script:fail++
        $msg = "  FAIL  " + $name
        if ($detail) { $msg += "  [" + $detail + "]" }
        Write-Output $msg
    }
}

if (-not (Test-Path $ExePath)) {
    Write-Output ("SKIP: app binary not built: " + $ExePath)
    Write-Output "      build it with windows\VirtualHIDTyper\build_app.ps1 first."
    exit 2
}

$sigRead = '[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint GetPrivateProfileString(string s, string k, string d, System.Text.StringBuilder r, uint n, string f);'
$sigWrite = '[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'
try { Add-Type -MemberDefinition $sigRead  -Name TdIniRead  -Namespace TdIniNs | Out-Null } catch { }
try { Add-Type -MemberDefinition $sigWrite -Name TdIniWrite -Namespace TdIniNs | Out-Null } catch { }

function ReadSec([string]$path, [string]$section, [string]$key) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][TdIniNs.TdIniRead]::GetPrivateProfileString($section, $key, "<MISSING>", $sb, 256, $path)
    return $sb.ToString()
}
function ReadKey([string]$path, [string]$key) { return ReadSec $path "Main" $key }
function SectionNames([string]$path) {
    return @(Get-Content $path | Where-Object { $_ -match "^\[" } | ForEach-Object { $_.Trim() })
}
function HasBom([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    return ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
}
function WriteIni([string]$path, [string]$text, [bool]$bom) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($bom)))
}
function NewCase([string]$name) {
    $d = Join-Path ([System.IO.Path]::GetTempPath()) ("td_ini_" + $name)
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    return $d
}

# Runs the app against a data dir and enforces the smoke contract.
function RunSmoke([string]$dataDir) {
    $log = Join-Path $dataDir "smoke.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $ExePath -WorkingDirectory $repo -ArgumentList @("--smoke", $log, ("--data-dir=" + $dataDir)) -PassThru
    $exited = $p.WaitForExit($SmokeTimeoutMs)
    $sw.Stop()
    $timedOut = -not $exited
    if ($timedOut) {
        try { $p.Kill(); $p.WaitForExit(5000) | Out-Null } catch { }
    }
    $exitCode = -1
    try { $exitCode = $p.ExitCode } catch { }
    $text = ""
    if (Test-Path $log) { $text = (Get-Content $log -Raw) }
    $marker = ($text -match "smoke: closing")
    return [pscustomobject]@{
        Ok        = ((-not $timedOut) -and ($exitCode -eq 0) -and $marker)
        ExitCode  = $exitCode
        TimedOut  = $timedOut
        Marker    = $marker
        ElapsedMs = $sw.ElapsedMilliseconds
        LogText   = $text
        LogPath   = $log
    }
}

function AssertSmoke([string]$label, $res) {
    Check ($label + ": exited within timeout")   (-not $res.TimedOut) ("elapsed " + $res.ElapsedMs + "ms")
    Check ($label + ": exit code is 0")          ($res.ExitCode -eq 0) ("exit " + $res.ExitCode)
    Check ($label + ": completion marker present") ($res.Marker) "smoke: closing"
}

$dup = "[Main]" + $NL + "IntervalMs=25" + $NL + "HotkeyStart=7,120" + $NL + "HotkeyStop=7,121" + $NL + "HotkeySchema=2" + $NL + "[Main]" + $NL + "IntervalMs=40" + $NL + "HotkeyStart=2,37" + $NL + "HotkeyStop=2,39" + $NL + "Esp32Port=COM6" + $NL

Write-Output "settings.ini normalisation"

# --- 1. BOM + a single [Main] ------------------------------------------
# This is the state that reads NO values at all: the BOM bytes glue onto the
# first section name so the header is not recognised. After repair the file
# must have no BOM and its values must be readable.
$d1 = NewCase "bom_one"
$f1 = Join-Path $d1 "settings.ini"
$t1 = "[Main]" + $NL + "IntervalMs=33" + $NL + "HotkeyStart=7,120" + $NL + "HotkeyStop=7,121" + $NL + "HotkeySchema=2" + $NL
WriteIni $f1 $t1 $true
Check "BOM+one: values unreadable before repair" ((ReadKey $f1 "HotkeyStart") -eq "<MISSING>") ("got " + (ReadKey $f1 "HotkeyStart"))
$r1 = RunSmoke $d1
AssertSmoke "BOM+one" $r1
Check "BOM+one: [Main] still the only section" ((SectionNames $f1) -join "|" -eq "[Main]")
Check "BOM+one: byte-order mark removed" (-not (HasBom $f1))
Check "BOM+one: values readable after repair" ((ReadKey $f1 "HotkeyStart") -eq "7,120") ("got " + (ReadKey $f1 "HotkeyStart"))
Check "BOM+one: IntervalMs preserved" ((ReadKey $f1 "IntervalMs") -eq "33")

# --- 2. BOM + duplicate [Main] ----------------------------------------
$d2 = NewCase "bom_dup"
$f2 = Join-Path $d2 "settings.ini"
WriteIni $f2 $dup $true
$b2s = ReadKey $f2 "HotkeyStart"
$b2i = ReadKey $f2 "IntervalMs"
$r2 = RunSmoke $d2
AssertSmoke "BOM+dup" $r2
Check "BOM+dup: [Main] collapsed" ((SectionNames $f2) -join "|" -eq "[Main]")
Check "BOM+dup: byte-order mark removed" (-not (HasBom $f2))
Check "BOM+dup: HotkeyStart preserved" ((ReadKey $f2 "HotkeyStart") -eq $b2s) ("was " + $b2s + " now " + (ReadKey $f2 "HotkeyStart"))
Check "BOM+dup: IntervalMs preserved" ((ReadKey $f2 "IntervalMs") -eq $b2i)
Check "BOM+dup: second-section key kept" ((ReadKey $f2 "Esp32Port") -eq "COM6")

# --- 3. No BOM + duplicate [Main] -------------------------------------
$d3 = NewCase "nobom_dup"
$f3 = Join-Path $d3 "settings.ini"
WriteIni $f3 $dup $false
$b3s = ReadKey $f3 "HotkeyStart"
$r3 = RunSmoke $d3
AssertSmoke "noBOM+dup" $r3
Check "noBOM+dup: [Main] collapsed" ((SectionNames $f3) -join "|" -eq "[Main]")
Check "noBOM+dup: HotkeyStart preserved (first section won)" ((ReadKey $f3 "HotkeyStart") -eq $b3s) ("was " + $b3s + " now " + (ReadKey $f3 "HotkeyStart"))
Check "noBOM+dup: unique key kept" ((ReadKey $f3 "Esp32Port") -eq "COM6")

# --- 4. More than one duplicated section name -------------------------
# [Main] and [Window] are both duplicated; each must collapse independently
# and no section may be lost.
$d4 = NewCase "multi_dup"
$f4 = Join-Path $d4 "settings.ini"
$t4 = "[Main]" + $NL + "IntervalMs=25" + $NL + "HotkeyStart=7,120" + $NL + "HotkeyStop=7,121" + $NL +
       "[Window]" + $NL + "Rect=10,10,800,600" + $NL + "Maximized=0" + $NL +
       "[Main]" + $NL + "IntervalMs=40" + $NL + "Esp32Port=COM7" + $NL +
       "[Window]" + $NL + "Rect=75,0,1115,680" + $NL + "Maximized=1" + $NL
WriteIni $f4 $t4 $false
$m4 = ReadKey $f4 "HotkeyStart"
$w4 = ReadSec $f4 "Window" "Rect"
$r4 = RunSmoke $d4
AssertSmoke "multi-dup" $r4
$secs4 = SectionNames $f4
Check "multi-dup: [Main] appears once" (@($secs4 | Where-Object { $_ -eq "[Main]" }).Count -eq 1) ($secs4 -join "|")
Check "multi-dup: [Window] appears once" (@($secs4 | Where-Object { $_ -eq "[Window]" }).Count -eq 1) ($secs4 -join "|")
Check "multi-dup: Main values preserved" ((ReadKey $f4 "HotkeyStart") -eq $m4) ("was " + $m4 + " now " + (ReadKey $f4 "HotkeyStart"))
Check "multi-dup: Window values preserved" ((ReadSec $f4 "Window" "Rect") -eq $w4) ("was " + $w4 + " now " + (ReadSec $f4 "Window" "Rect"))

# --- 5. Duplicate keys that differ only in case -----------------------
# The profile API compares key names case-insensitively, so "[main]" and
# "[MAIN]" are the same section to Windows. The repair must agree, otherwise
# it would leave two sections the API then treats as one.
$d5 = NewCase "case_dup"
$f5 = Join-Path $d5 "settings.ini"
$t5 = "[Main]" + $NL + "IntervalMs=25" + $NL + "HotkeyStart=7,120" + $NL + "HotkeyStop=7,121" + $NL +
       "[main]" + $NL + "IntervalMs=40" + $NL + "Esp32Port=COM9" + $NL
WriteIni $f5 $t5 $false
$r5 = RunSmoke $d5
AssertSmoke "case-dup" $r5
$secs5 = @(SectionNames $f5)
$mainCount = @($secs5 | Where-Object { $_ -match "^\[(?i)main\]$" }).Count
Check "case-dup: case-differing sections collapse to one" ($mainCount -eq 1) ($secs5 -join "|")
Check "case-dup: values still readable" ((ReadKey $f5 "HotkeyStart") -eq "7,120") ("got " + (ReadKey $f5 "HotkeyStart"))

# --- 6. Repeated save/load cycles in the same process ------------------
# A single smoke process performs one load and one save. The defect was about
# write/read disagreement, which can survive a single cycle, so drive several
# write-then-read rounds in one process via the profile API directly and check
# that the value the app will read is the value that was just written.
$d6 = NewCase "cycles"
$f6 = Join-Path $d6 "settings.ini"
WriteIni $f6 $dup $true
$r6 = RunSmoke $d6
AssertSmoke "cycles(seed)" $r6
$cyclesOk = $true
$cycleDetail = ""
for ($i = 0; $i -lt 6; $i++) {
    $v = "8," + (200 + $i)
    [void][TdIniNs.TdIniWrite]::WritePrivateProfileString("Main", "HotkeyStart", $v, $f6)
    $back = ReadKey $f6 "HotkeyStart"
    if ($back -ne $v) { $cyclesOk = $false; $cycleDetail = ("cycle " + $i + " wrote " + $v + " read " + $back); break }
    $secs = @(SectionNames $f6 | Where-Object { $_ -eq "[Main]" }).Count
    if ($secs -ne 1) { $cyclesOk = $false; $cycleDetail = ("cycle " + $i + " produced " + $secs + " [Main] sections"); break }
}
Check "cycles: six write/read rounds round-trip" $cyclesOk $cycleDetail

# --- 7. Failure-safe replacement --------------------------------------
# The repair must never destroy the only profile it has. Two invariants:
#   * no leftover .tmp file is left behind on the success path;
#   * a repair that cannot complete leaves the original bytes intact.
$d7 = NewCase "safe_replace"
$f7 = Join-Path $d7 "settings.ini"
WriteIni $f7 $dup $true
$r7 = RunSmoke $d7
AssertSmoke "safe-replace" $r7
$leftover = @(Get-ChildItem $d7 -Filter "*.tmp" -ErrorAction SilentlyContinue)
Check "safe-replace: no .tmp file left behind" ($leftover.Count -eq 0) (($leftover | ForEach-Object { $_.Name }) -join ",")

# Occupy the temp path with a directory so the temp write cannot succeed;
# the original file must survive byte-for-byte.
$d8 = NewCase "safe_fail"
$f8 = Join-Path $d8 "settings.ini"
WriteIni $f8 $dup $true
$before8 = [System.IO.File]::ReadAllBytes($f8)
New-Item -ItemType Directory -Force -Path ($f8 + ".tmp") | Out-Null
$r8 = RunSmoke $d8
$after8 = [System.IO.File]::ReadAllBytes($f8)
$same8 = ($before8.Length -eq $after8.Length)
if ($same8) { for ($i = 0; $i -lt $before8.Length; $i++) { if ($before8[$i] -ne $after8[$i]) { $same8 = $false; break } } }
Check "safe-fail: original profile untouched when replacement cannot be written" $same8 ("len " + $before8.Length + " -> " + $after8.Length)
Check "safe-fail: file still readable" ($after8.Length -gt 0) "non-empty"

# --- 8. [Window] and Esp32Port are covered by normalisation -------------
# RunSmoke writes the window rectangle on close and the ESP32 port on save.
# Both must survive a repair of a duplicated, BOM-prefixed profile.
$d9 = NewCase "window_port"
$f9 = Join-Path $d9 "settings.ini"
$t9 = "[Main]" + $NL + "IntervalMs=25" + $NL + "HotkeyStart=7,120" + $NL + "HotkeyStop=7,121" + $NL + "Esp32Port=COM6" + $NL +
       "[Window]" + $NL + "Rect=100,100,900,700" + $NL + "Maximized=0" + $NL +
       "[Main]" + $NL + "Esp32Port=COM6" + $NL +
       "[Window]" + $NL + "Rect=100,100,900,700" + $NL + "Maximized=0" + $NL
WriteIni $f9 $t9 $true
$r9 = RunSmoke $d9
AssertSmoke "window+port" $r9
Check "window+port: Esp32Port survives repair" ((ReadKey $f9 "Esp32Port") -eq "COM6") ("got " + (ReadKey $f9 "Esp32Port"))
$rect = ReadSec $f9 "Window" "Rect"
Check "window+port: [Window] Rect present after repair" ($rect -ne "<MISSING>") ("got " + $rect)
Check "window+port: [Window] still a single section" (@(SectionNames $f9 | Where-Object { $_ -eq "[Window]" }).Count -eq 1)

Write-Output ""
Write-Output ("RESULT: " + $script:pass + " passed, " + $script:fail + " failed")
if ($script:fail -gt 0) { exit 1 }
exit 0
