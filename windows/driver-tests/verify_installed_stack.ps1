# ---------------------------------------------------------------------------
# verify_installed_stack.ps1 -- post-install assertion suite for the TextDeck
# virtual HID keyboard.
#
# WHY THIS EXISTS
#   validate_ioctl_contract.py proves the *package* is correct: the INF declares
#   LowerFilters=vhf in the .HW section, with the right value type. That is a
#   statement about a file. It says nothing about what the PnP manager actually
#   built when that file was installed -- and the two can disagree:
#
#     * a device installed BEFORE the LowerFilters line existed keeps the
#       hardware key it was created with; re-running setup over the same
#       instance does not necessarily re-apply the .HW AddReg;
#     * pnputil /add-driver /install can report success while the device ends
#       up Code 31 because vhf.sys is not below it in the stack.
#
#   This script therefore checks the RUNNING device, not the package:
#   the device's own hardware key, the loaded driver stack, the device state,
#   and the VhfCreate status the driver itself recorded.
#
# EXIT CODES
#   0  PASS            every check executed and passed
#   1  FAIL            at least one check failed
#   2  NO DEVICE       no TextDeck instance exists (nothing was verified)
#   3  NOT SUPPORTED   not elevated / not Windows
#   4  PASS WITH SKIPS every executed check passed, but at least one did NOT run
#
# Why 4 exists: a caller that only tests `exit -ne 0` cannot distinguish "all
# checks passed" from "the checks that ran passed but one never ran". Treating
# those as the same result is how a skipped check becomes an unearned green.
# Callers that genuinely accept a skip can test for 0 or 4 explicitly.
#
# -Strict (automation aid): treats any skip as a failure, so an automated gate
# does not have to parse the free-form VERDICT line. In strict mode exit code 4
# is never returned; a skip yields 1.
#
# It is read-only: it never installs, removes, edits the registry or reboots.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$HardwareId = '',
    [string]$LogPath = '',
    # Fail instead of reporting PASS WITH SKIPS. For CI gates that must not
    # accept an unexecuted check.
    [switch]$Strict
)

# Default backfilled here rather than in the param block: when this script is run
# with `powershell.exe -File`, a default containing a backslash-quoted token was
# arriving as an empty string, and the script then reported "no device" for a
# device that was present -- a silent false negative in the very check that is
# supposed to catch silent failures.
if ([string]::IsNullOrWhiteSpace($HardwareId)) { $HardwareId = 'Root\TextDeckHidDriver' }

$ErrorActionPreference = 'Stop'
$script:Fail = 0
$script:Skipped = 0
$script:Passed = 0
$script:Lines = New-Object System.Collections.Generic.List[string]

function Emit($m) {
    $script:Lines.Add([string]$m)
    Write-Host $m
}
function Check($name, $cond, $detail) {
    if ($cond) { Emit ('  [PASS] ' + $name + ' :: ' + $detail); $script:Passed++ }
    else       { Emit ('  [FAIL] ' + $name + ' :: ' + $detail); $script:Fail++ }
}

if (-not $IsWindows -and $PSVersionTable.PSEdition -eq 'Core') { Emit 'not Windows'; exit 3 }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Emit 'must run elevated (the Enum keys and PnP properties need it)'
    exit 3
}

Emit '==== INSTALLED VHF STACK VERIFICATION ===='
Emit ('time       = ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Emit ('hardwareId = ' + $HardwareId)
Emit ''

# --- locate instances by exact hardware ID (never by a name guess) ----------
$instances = @()
$enumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT'
foreach ($devKey in @(Get-ChildItem $enumRoot -ErrorAction SilentlyContinue)) {
    if ($devKey.PSChildName -notlike 'TEXTDECKHIDDRIVER*') { continue }
    foreach ($inst in @(Get-ChildItem $devKey.PSPath -ErrorAction SilentlyContinue)) {
        $props = Get-ItemProperty $inst.PSPath -ErrorAction SilentlyContinue
        $hw = @(); if ($props.HardwareID) { $hw = @($props.HardwareID) }
        if ($hw -contains $HardwareId) {
            $instances += ('ROOT\' + $devKey.PSChildName + '\' + $inst.PSChildName)
        }
    }
}

if ($instances.Count -eq 0) {
    Emit ("NO DEVICE with hardware ID '" + $HardwareId + "' -- nothing verified (this is not a pass)")
    if ($LogPath) { $script:Lines -join "`r`n" | Set-Content -LiteralPath $LogPath -Encoding UTF8 }
    exit 2
}

Check 'exactly one device instance exists' ($instances.Count -eq 1) ($instances.Count.ToString() + ': ' + ($instances -join ', '))

foreach ($id in $instances) {
    Emit ''
    Emit ('--- ' + $id + ' ---')
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\' + $id
    $props = Get-ItemProperty $key -ErrorAction SilentlyContinue

    # 1. the device's OWN hardware key carries the filter
    $lf = $props.LowerFilters
    Check 'device hardware key has LowerFilters' ([bool]$lf) $(if ($lf) { $lf -join ';' } else { '<absent>' })
    Check 'LowerFilters contains vhf' ($lf -contains 'vhf') $(if ($lf) { $lf -join ';' } else { '<absent>' })

    $kind = '<absent>'
    $rk = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Enum\' + $id)
    if ($rk) { try { $kind = $rk.GetValueKind('LowerFilters').ToString() } catch {} ; $rk.Close() }
    Check 'LowerFilters is REG_MULTI_SZ' ($kind -eq 'MultiString') $kind

    # 2. the PnP property agrees
    $dlf = try { (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_LowerFilters' -ErrorAction Stop).Data } catch { $null }
    Check 'DEVPKEY_Device_LowerFilters contains vhf' ($dlf -contains 'vhf') $(if ($dlf) { $dlf -join ';' } else { '<none>' })

    # 3. vhf is ACTUALLY LOADED in the stack -- this is the check a registry
    #    value cannot substitute for
    $stack = try { (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_Stack' -ErrorAction Stop).Data } catch { $null }
    $stackStr = if ($stack) { $stack -join ' -> ' } else { '<none>' }
    Check 'vhf.sys is loaded in the device stack' ([bool]($stack -match '\\Driver\\vhf')) $stackStr
    Check 'the TextDeck driver is loaded in the stack' ([bool]($stack -match 'TextDeckHidDriver')) $stackStr

    # 4. device is started with no problem code
    $dev = Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue
    Check 'device status is OK' ($dev -and $dev.Status -eq 'OK') $(if ($dev) { $dev.Status } else { '<no device>' })
    $prob = try { (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data } catch { '?' }
    Check 'problem code is 0 (CM_PROB_NONE)' ("$prob" -eq '0') "$prob"

    # 5. the expected service is bound
    Check 'device service is TextDeckHidDriver' ($props.Service -eq 'TextDeckHidDriver') "$($props.Service)"
}

# --- the driver's own record of VhfCreate -----------------------------------
Emit ''
$diag = Get-ItemProperty 'HKLM:\SOFTWARE\TextDeckDiag' -ErrorAction SilentlyContinue
if ($diag -and ($diag.PSObject.Properties.Name -contains 'VhfCreateStatus')) {
    $vs = [uint32]$diag.VhfCreateStatus
    $ir = [uint32]$diag.VhfCreateIrql
    Check 'VhfCreate returned STATUS_SUCCESS' ($vs -eq 0) ('0x' + $vs.ToString('X8'))
    Check 'VhfCreate was called at PASSIVE_LEVEL' ($ir -eq 0) ("IRQL=$ir")
    Emit ('  (recorded by the driver: Stage=' + $diag.Stage + ' LastStage=' + $diag.LastStage + ')')
} else {
    # The key is REG_OPTION_VOLATILE: it disappears on reboot, so a driver that
    # loaded at boot leaves no record. That is a gap in evidence, not a failure --
    # but it must not be silently indistinguishable from a pass either. It is
    # counted separately and printed in the summary, so a reader can tell
    # "VhfCreate was proven" from "VhfCreate was not observed this boot".
    $script:Skipped++
    Emit '  [SKIP] VhfCreate status not recorded (volatile key absent -- driver loaded before this boot?)'
    Emit '         This is NOT a pass: the driver''s own VhfCreate result was not observed.'
}

# --- vhf service must be left at its in-box start type ----------------------
Emit ''
$qc = (sc.exe qc vhf 2>&1 | Out-String)
Check 'vhf service is DEMAND_START (in-box value)' ([bool]($qc -match '3\s+DEMAND_START')) (($qc -split "`n" | Select-String 'START_TYPE') -join ' ').Trim()

# --- the HID child the framework enumerated ---------------------------------
Emit ''
$kb = (pnputil /enum-devices /class Keyboard 2>&1 | Out-String)
Check 'VHF enumerated a HID keyboard child' ([bool]($kb -match 'VID_1209&PID_0001')) 'VID_1209&PID_0001 present in Keyboard class'

Emit ''
# Three distinct outcomes, because "no failures" is not the same statement as
# "every check ran". A reader who only sees ALL CHECKS PASSED must not conclude
# that the skipped VhfCreate observation was verified.
#
#   PASS            every check executed and passed
#   PASS WITH SKIPS every executed check passed, but at least one check did not
#                   run (the volatile diagnostic key is absent after a reboot)
#   FAIL            at least one check failed
$verdict = if ($script:Fail -gt 0) { 'FAIL' }
           elseif ($script:Skipped -gt 0) { 'PASS WITH SKIPS' }
           else { 'PASS' }

$detail = if ($script:Fail -gt 0) {
    "$($script:Fail) CHECK(S) FAILED"
} elseif ($script:Skipped -gt 0) {
    "$($script:Passed) passed, $($script:Skipped) SKIPPED -- the skipped check did NOT run and is NOT a pass"
} else {
    "$($script:Passed) checks passed, none skipped"
}

Emit ('==== VERDICT: ' + $verdict + ' ====')
Emit ('     ' + $detail)
if ($script:Skipped -gt 0) {
    Emit '     Skipped: the driver''s VhfCreate result was not observed this boot.'
    Emit '     To convert this to a plain PASS, restart the device so the driver'
    Emit '     re-records the volatile status, then run this script again:'
    Emit '       pnputil /restart-device "<instance id>"'
    Emit '     The instance id MUST be quoted: without quotes pnputil treats the'
    Emit '     backslash-separated id as its own argument and prints usage instead'
    Emit '     of restarting anything.'
}

if ($LogPath) { $script:Lines -join "`r`n" | Set-Content -LiteralPath $LogPath -Encoding UTF8 }

# Exit codes are the machine-readable contract; the VERDICT line is for humans.
#   FAIL -> 1, PASS WITH SKIPS -> 4 (or 1 under -Strict), PASS -> 0.
# -Strict never returns 4, so an automated gate that passes -Strict cannot end up
# accepting an unexecuted check by forgetting to test for 4.
if ($script:Fail -gt 0) {
    exit 1
}
if ($script:Skipped -gt 0) {
    if ($Strict) {
        Emit '     -Strict is set: a skipped check counts as a failure (exit 1).'
        $script:Lines -join "`r`n" | Set-Content -LiteralPath $LogPath -Encoding UTF8
        exit 1
    }
    exit 4
}
exit 0
