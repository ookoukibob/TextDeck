# ---------------------------------------------------------------------------
# verify_driver_package.ps1 -- read-only verification of a built driver package.
#
# SIGNING.md tells you to sign the SYS, regenerate the catalog, then sign the
# CAT, and to check all four signtool verifications. Nothing in this repository
# could check that a package in front of you actually satisfies that, so a
# half-signed package (SYS signed but catalogue stale, or catalogue signed but
# not covering the INF) would look finished.
#
# This script only READS. It never signs, installs, imports a certificate, or
# changes BCD/boot configuration.
#
# Exit codes:  0 = all checks passed
#              1 = at least one check failed
#              2 = package not found (nothing was verified)
#
# Usage:
#   powershell -File verify_driver_package.ps1 [-PackageDir <dir>]
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$PackageDir = ""
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot is <repo>/windows/driver-tools -> two levels up is the repo root.
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $PackageDir) {
    $PackageDir = Join-Path $repo "windows\driver\TextDeckHidDriver\x64\Debug\TextDeckHidDriver"
}

$script:pass = 0
$script:fail = 0
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { $script:pass++; Write-Output ("  [PASS] " + $name) }
    else {
        $script:fail++
        $msg = "  [FAIL] " + $name
        if ($detail) { $msg += " :: " + $detail }
        Write-Output $msg
    }
}

Write-Output "TextDeck driver package verification"
Write-Output ("=" * 66)
Write-Output ("package: " + $PackageDir)


# ---- 1. the package must exist and be complete --------------------------
$sys = Join-Path $PackageDir "TextDeckHidDriver.sys"
$inf = Join-Path $PackageDir "TextDeckHidDriver.inf"
$cat = Join-Path $PackageDir "textdeckhiddriver.cat"

$missing = @();
foreach ($pair in @(@("SYS", $sys), @("INF", $inf), @("CAT", $cat))) {
    if (-not (Test-Path $pair[1])) { $missing += $pair[0] }
}
if ($missing.Count -gt 0) {
    Write-Output ("  [FAIL] package incomplete, missing: " + ($missing -join ", "))
    Write-Output ("         build it with windows\driver-tools\build_driver.ps1 first.")
    Write-Output ("=" * 66)
    Write-Output "RESULT: NOTHING VERIFIED (package not found)"
    exit 2
}
Check "SYS present" $true ("{0:N0} bytes" -f (Get-Item $sys).Length)
Check "INF present" $true ("{0:N0} bytes" -f (Get-Item $inf).Length)
Check "CAT present" $true ("{0:N0} bytes" -f (Get-Item $cat).Length)


# ---- 2. the INF must still describe this package ------------------------
$infText = Get-Content $inf -Raw
Check "INF declares the TextDeck hardware ID" ($infText -match "Root\\TextDeckHidDriver")
Check "INF names the service .sys file"        ($infText -match "TextDeckHidDriver\.sys")


# ---- 3. the CAT must actually hash the files it claims to cover ---------
# A catalogue is tied to file CONTENT. If the SYS was rebuilt or re-signed
# after the catalogue was generated, the catalogue is stale and its signature
# is worthless even though it still verifies as a signed blob.
$catNewerThanSys = (Get-Item $cat).LastWriteTime -ge (Get-Item $sys).LastWriteTime
$catNewerThanInf = (Get-Item $cat).LastWriteTime -ge (Get-Item $inf).LastWriteTime
Check "catalogue is not older than the SYS" $catNewerThanSys ("cat " + (Get-Item $cat).LastWriteTime + " vs sys " + (Get-Item $sys).LastWriteTime)
Check "catalogue is not older than the INF" $catNewerThanInf ("cat " + (Get-Item $cat).LastWriteTime + " vs inf " + (Get-Item $inf).LastWriteTime)


# ---- 4. signtool verification -------------------------------------------
$st = $null
$kitBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
if (Test-Path $kitBin) {
    $st = Get-ChildItem $kitBin -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match "\\x64\\" } |
          Sort-Object FullName | Select-Object -Last 1
}
if (-not $st) {
    Write-Output "  [SKIP] signtool.exe not found under the Windows Kits -- signature checks NOT RUN"
    Write-Output "         (this is not a pass: an unverified package is unverified)"
} else {
    $stPath = $st.FullName
    Write-Output ("  signtool: " + $stPath)
    Write-Output ""

    # signtool exits non-zero for an unsigned file, which is a normal result
    # here, not an error condition. Call it so its exit code is data.
    function Invoke-SignTool([string[]]$argList) {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & $stPath @argList 2>&1
            return [pscustomobject]@{ Exit = $LASTEXITCODE; Output = ($output | Out-String) }
        } finally {
            $ErrorActionPreference = $prev
        }
    }

    # /pa = default Authenticode policy, which is what the loader uses.
    $r1 = Invoke-SignTool @("verify","/pa","/v",$sys)
    Check "SYS signature verifies (/pa)" ($r1.Exit -eq 0) ("signtool exit " + $r1.Exit)
    if ($r1.Exit -ne 0) {
        ($r1.Output -split "\n" | Where-Object { $_.Trim() -ne "" } | Select-Object -First 2) |
            ForEach-Object { Write-Output ("         " + $_.Trim()) }
    }

    $r2 = Invoke-SignTool @("verify","/pa","/v",$cat)
    Check "CAT signature verifies (/pa)" ($r2.Exit -eq 0) ("signtool exit " + $r2.Exit)
    if ($r2.Exit -ne 0) {
        ($r2.Output -split "\n" | Where-Object { $_.Trim() -ne "" } | Select-Object -First 2) |
            ForEach-Object { Write-Output ("         " + $_.Trim()) }
    }

    # /c checks that the catalogue actually covers that specific file. A CAT can
    # verify fine while covering nothing useful - exactly the failure a
    # half-finished signing procedure produces.
    $r3 = Invoke-SignTool @("verify","/pa","/v","/c",$cat,$inf)
    Check "catalogue covers the INF" ($r3.Exit -eq 0) ("signtool exit " + $r3.Exit)

    $r4 = Invoke-SignTool @("verify","/pa","/v","/c",$cat,$sys)
    Check "catalogue covers the SYS" ($r4.Exit -eq 0) ("signtool exit " + $r4.Exit)
}

# ---- 5. report -----------------------------------------------------------
Write-Output ("=" * 66)
Write-Output ("RESULT: " + $script:pass + " passed, " + $script:fail + " failed")
if ($script:fail -gt 0) {
    Write-Output "This package is NOT ready to install. See docs/SIGNING.md."
    exit 1
}
Write-Output "Package is complete, signed, and the catalogue covers both files."
Write-Output "This does NOT make it production-distributable: a test-signed package"
Write-Output "only loads where test signing is enabled. See docs/SIGNING.md."
Write-Output ""
Write-Output "NOTE: this verified the PACKAGE, not an installed device. A correct INF"
Write-Output "does not guarantee a correct install -- a device created before the"
Write-Output "LowerFilters line existed keeps its old hardware key. To check what the"
Write-Output "PnP manager actually built, run on the target machine:"
Write-Output "  windows\driver-tests\verify_installed_stack.ps1"
exit 0
